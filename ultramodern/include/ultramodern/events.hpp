#ifndef __EVENTS_HPP__
#define __EVENTS_HPP__

#include <cstdint>

namespace ultramodern {
    namespace events {
        struct callbacks_t {
            using vi_callback_t = void();
            using gfx_init_callback_t = void();
            using gfx_task_submitted_callback_t = void(std::uint8_t*, std::uint32_t, std::uint32_t);

            /**
             * Called in each VI.
             */
            vi_callback_t* vi_callback;

            /**
             * Called before entering the gfx main loop.
             */
            gfx_init_callback_t* gfx_init_callback;

            /** Called on the game thread before a graphics task is queued. */
            gfx_task_submitted_callback_t* gfx_task_submitted_callback;
        };

        void set_callbacks(const callbacks_t& callbacks);
    }
}

#endif
