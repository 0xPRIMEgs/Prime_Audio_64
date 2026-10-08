
#include <libdragon.h>
#include "prime_audio_64.c"

// Sound effects
static wav64_t sfx_hello;

int main(void) {
    // Initialize N64 systems
    display_init(
        RESOLUTION_320x240,
        DEPTH_16_BPP,
        2,
        GAMMA_NONE,
        FILTERS_DISABLED
    );

    controller_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    // Initialize Prime Audio
    prime_audio_init();

    // Load sounds
    load_snd(&sfx_hello, "rom:/hello.wav64");
    changeBGM("rom:/music.wav64");

    // Set volumes
    setMasterVolume(1.0f);
    setBGMVolume(0.4f);
    setSFXVolume(0.7f);

    while (1) {
        controller_scan();
        struct controller_data keys = get_keys_down();

        // Press A to play sound
        if (keys.c[0].A) {
            play_snd(&sfx_hello);
        }

        // Update audio EVERY frame
        prime_audio_update();

        // Render
        surface_t *disp = display_get();
        graphics_fill_screen(disp, 0);
        graphics_set_color(
            graphics_make_color(255, 255, 255, 255),
            0
        );
        graphics_draw_text(disp, 20, 20, "PRIME AUDIO 64");
        graphics_draw_text(disp, 20, 40, "Press A to play SFX");
        display_show(disp);
    }

    // Cleanup if you ever exit the loop
    prime_audio_close();
    return 0;
}
