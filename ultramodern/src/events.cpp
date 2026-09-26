#include <thread>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <utility>
#include <mutex>
#include <queue>
#include <cstring>
#include <cstdio>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/extensions.h"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
static uint32_t dora64_dl_hash(const uint8_t* bytes) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < 0x1000; i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}
#endif

struct SpTaskAction {
    OSTask task;
#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
    uint32_t debug_nested_addr = 0;
    uint32_t debug_nested_hash = 0;
    std::chrono::steady_clock::time_point debug_submitted_at{};
#endif
};

struct ScreenUpdateAction {
    ultramodern::renderer::ViRegs regs;
    bool cpu_changes_only = false;
};

struct UpdateConfigAction {
};

struct DummyWorkloadAction {
    int32_t fb_address;
};

struct GameResetBarrierAction {
    moodycamel::LightweightSemaphore* completed;
};

using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction, DummyWorkloadAction, GameResetBarrierAction>;

struct ViState {
    const OSViMode* mode;
    PTR(void) framebuffer;
    PTR(OSMesg) mq;
    OSMesg msg;
    uint32_t state;
    uint32_t control;
    int retrace_count = 1;
};

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

static struct {
    struct {
        std::thread thread;
        int cur_state;
        int field;
        ViState states[2];
        ultramodern::renderer::ViRegs regs;
        ultramodern::renderer::ViRegs update_screen_regs;
        std::mutex regs_mutex;

        ViState* get_next_state() {
            return &states[cur_state ^ 1];
        }
        ViState* get_cur_state() {
            return &states[cur_state];
        }
        void update_vi() {
            ViState* next_state = get_next_state();
            const OSViMode* next_mode = next_state->mode;
            const OSViCommonRegs* common_regs = &next_mode->comRegs;
            const OSViFieldRegs* field_regs = &next_mode->fldRegs[field];
            PTR(void) framebuffer = osVirtualToPhysical(next_state->framebuffer);
            PTR(void) origin = framebuffer + field_regs->origin;

            // Process the VI state flags.
            uint32_t hStart = common_regs->hStart;
            if (next_state->state & VI_STATE_BLACK) {
                hStart = 0;
            }

            uint32_t yScale = field_regs->yScale;
            if (next_state->state & VI_STATE_REPEATLINE) {
                yScale = 0;
                origin = framebuffer;
            }

            // TODO implement osViFade

            // Update VI registers.
            regs.VI_ORIGIN_REG = origin;
            regs.VI_WIDTH_REG = common_regs->width;
            regs.VI_TIMING_REG = common_regs->burst;
            regs.VI_V_SYNC_REG = common_regs->vSync;
            regs.VI_H_SYNC_REG = common_regs->hSync;
            regs.VI_LEAP_REG = common_regs->leap;
            regs.VI_H_START_REG = hStart;
            regs.VI_V_START_REG = field_regs->vStart; // TODO implement osViExtendVStart
            regs.VI_V_BURST_REG = field_regs->vBurst;
            regs.VI_INTR_REG = field_regs->vIntr;
            regs.VI_X_SCALE_REG = common_regs->xScale; // TODO implement osViSetXScale
            regs.VI_Y_SCALE_REG = yScale; // TODO implement osViSetYScale
            regs.VI_STATUS_REG = next_state->control;
            
            // Swap VI states.
            cur_state ^= 1;
            *get_next_state() = *get_cur_state();
        }
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    // PI DMA can overwrite display-list resources as soon as the game receives SP completion.
    // Keep those writes from racing the HLE display-list parser.
    std::mutex graphics_rdram_mutex;
    uint8_t* rdram;
    // Doraemon finishes its CPU framebuffer overlay from the scheduler thread.
    // Once that hook is active, it becomes the single screen-update boundary.
    std::atomic_bool scheduler_screen_update_active = false;
    std::atomic_bool game_reset_paused = false;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
} events_context{};

std::mutex& ultramodern::get_graphics_rdram_mutex() {
    return events_context.graphics_rdram_mutex;
}

ultramodern::renderer::ViRegs* ultramodern::renderer::get_vi_regs() {
    return &events_context.vi.update_screen_regs;
}

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    std::lock_guard lock{ events_context.message_mutex };

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
    }
}

extern "C" void osViSetEvent(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, u32 retrace_count) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mq = mq_;
    next_state->msg = msg;
    next_state->retrace_count = retrace_count;
}

uint64_t total_vis = 0;


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi(bool odd);

void vi_thread_func() {
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    int remaining_retraces = 1;

    while (!exited) {
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        auto time_now = ultramodern::time_since_start();
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (time_now * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        if (events_context.game_reset_paused.load(std::memory_order_acquire)) {
            continue;
        }

        // If the game hasn't started yet, set a dummy VI mode and origin.
        if (!ultramodern::is_game_started()) {
            static bool odd = false;
            set_dummy_vi(odd);
            odd = !odd;

            events_context.action_queue.enqueue(DummyWorkloadAction{events_context.vi.get_next_state()->framebuffer});
        }

        {
            std::lock_guard lock{ events_context.vi.regs_mutex };

            // Before Doraemon's scheduler hook becomes active, retain the
            // runtime's normal VI-driven screen updates. Afterwards the hook
            // queues the update after the complete CPU text/menu overlay, so
            // an intermediate framebuffer without text is never presented.
            if (!events_context.scheduler_screen_update_active.load(std::memory_order_acquire)) {
                events_context.action_queue.enqueue(ScreenUpdateAction{ events_context.vi.regs });
            }

            // Update VI registers and swap VI modes.
            events_context.vi.update_vi();
        }

        // If the game has started, handle sending VI and AI events.
        if (ultramodern::is_game_started()) {
            remaining_retraces--;
            
            std::lock_guard lock{ events_context.message_mutex };
            ViState* cur_state = events_context.vi.get_cur_state();
            if (remaining_retraces == 0) {
                if (cur_state->mq != NULLPTR) {
                    // Send a message to the VI queue, and do not set it to be requeued if the queue was full.
                    // The worst case scenario is that the game misses a VI message and has to wait a little longer for the next. 
                    ultramodern::enqueue_external_message_src(cur_state->mq, cur_state->msg, false, ultramodern::EventMessageSource::Vi);
                }
                remaining_retraces = cur_state->retrace_count;
            }
            if (events_context.ai.mq != NULLPTR) {
                // Send a message to the VI queue, and do not set it to be requeued if the queue was full for the same reason as the VI message above.
                ultramodern::enqueue_external_message_src(events_context.ai.mq, events_context.ai.msg, false, ultramodern::EventMessageSource::Ai);
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

void sp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    ultramodern::enqueue_external_message_src(events_context.sp.mq, events_context.sp.msg, false, ultramodern::EventMessageSource::Sp);
}

void dp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    ultramodern::enqueue_external_message_src(events_context.dp.mq, events_context.dp.msg, false, ultramodern::EventMessageSource::Dp);
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
    ultramodern::set_native_thread_name("SP Task Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }

        if (task == reinterpret_cast<OSTask*>(1)) {
            extern moodycamel::LightweightSemaphore* game_reset_sp_barrier;
            game_reset_sp_barrier->signal();
            continue;
        }

        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            fprintf(stderr, "Failed to execute task type: %" PRIu32 "\n", task->t.type);
            ULTRAMODERN_QUICK_EXIT();
        }

        // Tell the game that the RSP has completed
        sp_complete();
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto& config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::get_graphics_config().developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (!exited) {
        // Try to pull an action from the queue
        Action action;
        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                std::unique_lock rdram_lock{ events_context.graphics_rdram_mutex };
                ultramodern::measure_input_latency();
#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
                const auto queue_delay = std::chrono::steady_clock::now() - task_action->debug_submitted_at;
                if (task_action->debug_nested_addr != 0) {
                    const uint32_t current_hash = dora64_dl_hash(rdram + task_action->debug_nested_addr);
                    if (current_hash != task_action->debug_nested_hash) {
                        std::fprintf(stderr, "Dora64 DL changed before parse: target=%08X submit=%08X parse=%08X task=%08X\n",
                            task_action->debug_nested_addr, task_action->debug_nested_hash,
                            current_hash, task_action->task.t.data_ptr);
                        std::fflush(stderr);
                    }
                }
#endif

                // Keep the original RSP timing; delaying this until parsing completes
                // did not prevent Doraemon's nested-list corruption and stalls the game.
                sp_complete();
                PTR(u64) displaylist = task_action->task.t.data_ptr;
                ultramodern::extensions::on_displaylist_submitted(displaylist);

                [[maybe_unused]] auto renderer_start = std::chrono::high_resolution_clock::now();
                renderer_context->send_dl(&task_action->task);
                [[maybe_unused]] auto renderer_end = std::chrono::high_resolution_clock::now();
#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
                static uint32_t gfx_samples = 0;
                static uint32_t gfx_tracked = 0;
                static uint64_t gfx_queue_total_us = 0, gfx_parse_total_us = 0;
                static uint64_t gfx_queue_max_us = 0, gfx_parse_max_us = 0;
                const uint64_t queue_us = std::chrono::duration_cast<std::chrono::microseconds>(queue_delay).count();
                const uint64_t parse_us = std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count();
                gfx_queue_total_us += queue_us;
                gfx_parse_total_us += parse_us;
                gfx_queue_max_us = std::max(gfx_queue_max_us, queue_us);
                gfx_parse_max_us = std::max(gfx_parse_max_us, parse_us);
                gfx_tracked += (task_action->debug_nested_addr != 0);
                if (++gfx_samples == 30) {
                    std::fprintf(stderr, "Dora64 gfx 30 tasks: tracked=%u queue avg=%llu max=%llu us, parse avg=%llu max=%llu us\n",
                        gfx_tracked,
                        (unsigned long long)(gfx_queue_total_us / gfx_samples), (unsigned long long)gfx_queue_max_us,
                        (unsigned long long)(gfx_parse_total_us / gfx_samples), (unsigned long long)gfx_parse_max_us);
                    std::fflush(stderr);
                    gfx_samples = gfx_tracked = 0;
                    gfx_queue_total_us = gfx_parse_total_us = gfx_queue_max_us = gfx_parse_max_us = 0;
                }
#endif
                rdram_lock.unlock();

                dp_complete();
                // TODO hook the parsed event up to the actual parsing point when a callback is added to RT64.
                ultramodern::extensions::on_displaylist_parsed(displaylist);
                ultramodern::extensions::on_displaylist_completed(displaylist);
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* screen_update_action = std::get_if<ScreenUpdateAction>(&action)) {
                // A dialogue hook runs from the scheduler after it has switched
                // to the new current framebuffer. Use the VI snapshot captured
                // by that hook rather than the previous retrace's registers.
                events_context.vi.update_screen_regs = screen_update_action->regs;
#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
                const auto screen_start = std::chrono::steady_clock::now();
#endif
                renderer_context->update_screen(screen_update_action->cpu_changes_only);
#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
                static uint32_t screen_samples = 0;
                static uint64_t screen_total_us = 0, screen_max_us = 0;
                const uint64_t screen_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - screen_start).count();
                screen_total_us += screen_us;
                screen_max_us = std::max(screen_max_us, screen_us);
                if (++screen_samples == 30) {
                    std::fprintf(stderr, "Dora64 screen 30 updates: avg=%llu max=%llu us\n",
                        (unsigned long long)(screen_total_us / screen_samples), (unsigned long long)screen_max_us);
                    std::fflush(stderr);
                    screen_samples = 0;
                    screen_total_us = screen_max_us = 0;
                }
#endif
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                (void)config_action;
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
            else if (const auto* dummy_workload_action = std::get_if<DummyWorkloadAction>(&action)) {
                renderer_context->send_dummy_workload(dummy_workload_action->fb_address);
            }
            else if (const auto* reset_action = std::get_if<GameResetBarrierAction>(&action)) {
                // The host renderer survives an in-process game reset. Reset
                // its emulated RSP/RDP session after all old display lists have
                // been parsed and before the guest RDRAM is cleared.
                renderer_context->reset_game();
                reset_action->completed->signal();
            }
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

static const OSViMode dummy_mode = []() {
    OSViMode ret{};

    ret.type = 2;
    ret.comRegs.ctrl = VI_CTRL_TYPE_16 | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_GAMMA_ON | VI_CTRL_DIVOT_ON | VI_CTRL_ANTIALIAS_MODE_1 | VI_CTRL_PIXEL_ADV_3;
    ret.comRegs.width = 0x140;
    ret.comRegs.burst = 0x03E52239;
    ret.comRegs.vSync = 0x20D;
    ret.comRegs.hSync = 0xC15;
    ret.comRegs.leap = 0x0C150C15;
    ret.comRegs.hStart = 0x006C02EC;
    ret.comRegs.xScale = 0x200;
    ret.comRegs.vCurrent = 0x0;

    for (int field = 0; field < 2; field++) {
        ret.fldRegs[field].origin = 0x280;
        ret.fldRegs[field].yScale = 0x400;
        ret.fldRegs[field].vStart = 0x2501FF;
        ret.fldRegs[field].vBurst = 0xE0204;
        ret.fldRegs[field].vIntr = 0x2;
    }

    return ret;
}();

void set_dummy_vi(bool odd) {
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = &dummy_mode;
    next_state->control = next_state->mode->comRegs.ctrl;
    // Set up a dummy framebuffer.
    next_state->framebuffer = 0x80700000;
    if (odd) {
        next_state->framebuffer += 0x25800;
    }
}

moodycamel::LightweightSemaphore* game_reset_sp_barrier = nullptr;

void ultramodern::reset_events_for_game_reset(RDRAM_ARG1) {
    events_context.game_reset_paused.store(true, std::memory_order_release);

    // Finish all RSP work submitted by the old session before its RDRAM is
    // cleared. The sentinel is ordered after the previously queued tasks.
    moodycamel::LightweightSemaphore sp_completed;
    game_reset_sp_barrier = &sp_completed;
    events_context.sp_task_queue.enqueue(reinterpret_cast<OSTask*>(1));
    sp_completed.wait();
    game_reset_sp_barrier = nullptr;

    // Do the same for graphics and screen-update work.
    moodycamel::LightweightSemaphore gfx_completed;
    events_context.action_queue.enqueue(GameResetBarrierAction{&gfx_completed});
    gfx_completed.wait();

    {
        std::scoped_lock lock{events_context.message_mutex, events_context.vi.regs_mutex};
        events_context.sp.mq = NULLPTR;
        events_context.sp.msg = 0;
        events_context.dp.mq = NULLPTR;
        events_context.dp.msg = 0;
        events_context.ai.mq = NULLPTR;
        events_context.ai.msg = 0;
        events_context.si.mq = NULLPTR;
        events_context.si.msg = 0;

        ViState clean_vi{};
        clean_vi.mode = &dummy_mode;
        clean_vi.framebuffer = 0x80700000;
        clean_vi.control = dummy_mode.comRegs.ctrl;
        clean_vi.retrace_count = 1;
        events_context.vi.cur_state = 0;
        events_context.vi.field = 0;
        events_context.vi.states[0] = clean_vi;
        clean_vi.framebuffer = 0x80725800;
        events_context.vi.states[1] = clean_vi;
        events_context.vi.regs = {};
        events_context.vi.update_screen_regs = {};
    }

    events_context.scheduler_screen_update_active.store(false, std::memory_order_release);
}

void ultramodern::resume_events_after_game_reset() {
    events_context.game_reset_paused.store(false, std::memory_order_release);
}

extern "C" void osViSwapBuffer(RDRAM_ARG PTR(void) frameBufPtr) {
    std::lock_guard lock{ events_context.message_mutex };
    events_context.vi.get_next_state()->framebuffer = frameBufPtr;
}

extern "C" void osViSetMode(RDRAM_ARG PTR(OSViMode) mode_) {
    std::lock_guard lock{ events_context.message_mutex };
    OSViMode* mode = TO_PTR(OSViMode, mode_);
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = mode;
    next_state->control = next_state->mode->comRegs.ctrl;
}

#define OS_VI_GAMMA_ON          0x0001
#define OS_VI_GAMMA_OFF         0x0002
#define OS_VI_GAMMA_DITHER_ON   0x0004
#define OS_VI_GAMMA_DITHER_OFF  0x0008
#define OS_VI_DIVOT_ON          0x0010
#define OS_VI_DIVOT_OFF         0x0020
#define OS_VI_DITHER_FILTER_ON  0x0040
#define OS_VI_DITHER_FILTER_OFF 0x0080

extern "C" void osViSetSpecialFeatures(uint32_t func) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* control_out = &next_state->control;
    if ((func & OS_VI_GAMMA_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_DIVOT_ON) != 0) {
        *control_out |= VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DIVOT_OFF) != 0) {
        *control_out &= ~VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DITHER_FILTER_ON) != 0) {
        *control_out |= VI_CTRL_DITHER_FILTER_ON;
        *control_out &= ~VI_CTRL_ANTIALIAS_MASK;
    }

    if ((func & OS_VI_DITHER_FILTER_OFF) != 0) {
        *control_out &= ~VI_CTRL_DITHER_FILTER_ON;
        *control_out |= next_state->mode->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
}

extern "C" void osViBlack(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_BLACK;
    } else {
        *state_out &= ~VI_STATE_BLACK;
    }
}

extern "C" void osViRepeatLine(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_REPEATLINE;
    } else {
        *state_out &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" PTR(void) osViGetNextFramebuffer() {
    return events_context.vi.get_next_state()->framebuffer;
}

extern "C" PTR(void) osViGetCurrentFramebuffer() {
    return events_context.vi.get_cur_state()->framebuffer;
}

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    // Send gfx tasks to the graphics action queue
    if (task->t.type == M_GFXTASK) {
#if defined(__ANDROID__)
        // PI DMA holds this mutex while copying ROM data into RDRAM. Take the
        // same lock for the entire snapshot so a graphics task cannot capture
        // half of an old display list and half of the next resource bank.
        std::unique_lock rdram_lock{ events_context.graphics_rdram_mutex };
#endif
        if (events_callbacks.gfx_task_submitted_callback != nullptr) {
            events_callbacks.gfx_task_submitted_callback(rdram, task->t.data_ptr, task->t.data_size);
        }
        SpTaskAction action{ *task };
#if defined(__ANDROID__) && defined(DORA64_ANDROID_DIAGNOSTICS)
        const uint32_t dl_start = task->t.data_ptr & 0x7FFFFFu;
        if (dl_start <= 0x800000u - 0x120u) {
            uint32_t caller_w0, caller_w1;
            std::memcpy(&caller_w0, rdram + dl_start + 0x118u, sizeof(caller_w0));
            std::memcpy(&caller_w1, rdram + dl_start + 0x11Cu, sizeof(caller_w1));
            if (caller_w0 == 0x06000000u) {
                const uint32_t target = caller_w1 & 0x7FFFFFu;
                if (target != 0 && target <= 0x800000u - 0x1000u) {
                    action.debug_nested_addr = target;
                    action.debug_nested_hash = dora64_dl_hash(rdram + target);
                }
            }
        }
        action.debug_submitted_at = std::chrono::steady_clock::now();
#endif
        events_context.action_queue.enqueue(std::move(action));
    }
    // Set all other tasks as the RSP task
    else {
        events_context.sp_task_queue.enqueue(task);
    }
}

extern "C" void ultramodern_notify_cpu_framebuffer_write(uint8_t*) {
    // Doraemon calls this once per retrace after compositing its complete CPU
    // text/menu overlay and before submitting work for the next frame. Make
    // this the sole presentation boundary instead of presenting both the
    // pre-overlay and post-overlay versions of the framebuffer.
    events_context.scheduler_screen_update_active.store(true, std::memory_order_release);

    ultramodern::renderer::ViRegs current_vi_regs;
    {
        std::lock_guard lock{ events_context.vi.regs_mutex };
        current_vi_regs = events_context.vi.regs;
    }
    events_context.action_queue.enqueue(ScreenUpdateAction{ current_vi_regs });
}

void ultramodern::send_si_message() {
    ultramodern::enqueue_external_message_src(events_context.si.mq, events_context.si.msg, false, ultramodern::EventMessageSource::Si);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsPipelineCreationFailed:
                show_renderer_error("The graphics driver could not create the required shaders, including the compatibility path where applicable. Please share the Android log with the Dora64 developers.");
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

    events_context.vi.thread = std::thread{ vi_thread_func };
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}
