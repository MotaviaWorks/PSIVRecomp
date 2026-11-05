package com.psilok.psiv;

import org.libsdl.app.SDLActivity;

/**
 * The game's activity: SDL3's own, loading libSDL3.so and then libmain.so
 * (the port, runtime/main_sdl.c), whose SDL_main runs the game.
 */
public class PsivActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
