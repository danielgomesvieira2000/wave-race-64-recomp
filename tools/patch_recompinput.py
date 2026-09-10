"""Let the port assign controllers to players without the modal.

RecompFrontend assigns players one way: `playerassignment::start()` opens a
modal, each player presses a button on the pad they want, and
`commit_player_assignment()` writes the result. That is the right flow for a
four-player game where who is who matters. For a game where the first pad is
player one and the second is player two, it is a screen to get through before
the first race, every time a pad is plugged in -- and until it has been got
through, nothing is assigned, so the pad drives the game (which the port reads
itself) while rumble, which goes through the player list, does nothing.

This adds `players::auto_assign_controllers`, which does what committing a
manual assignment does -- fill the player list in order and give each player the
profile that belongs to its controller -- from a list the caller supplies rather
than from button presses. It refuses while a manual assignment is open, so the
modal still wins where someone has chosen to use it.

Nothing upstream changes behaviour: the function is only what the port calls.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently.

Run from the repository root:
    python tools/patch_recompinput.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "lib" / "RecompFrontend" / "recompinput" / "include" / "recompinput" / "players.h"
SOURCE = REPO / "lib" / "RecompFrontend" / "recompinput" / "src" / "players.cpp"

HEADER_ANCHOR = """        InputDevice get_player_input_device(int player_index, bool temp_player = false);
    }"""

HEADER_REPLACEMENT = """        InputDevice get_player_input_device(int player_index, bool temp_player = false);

        // Assigns the given controllers to players in order, up to the maximum
        // number of players, as though they had been assigned through the modal
        // and committed. Passing none assigns the keyboard to player one, so a
        // machine with no pad attached still has a player. Does nothing while a
        // manual assignment is open.
        void auto_assign_controllers(SDL_GameController* const* controllers, size_t count);
    }"""

SOURCE_ANCHOR = """// playerassignment start"""

SOURCE_REPLACEMENT = """void players::auto_assign_controllers(SDL_GameController* const* controllers, size_t count) {
    if (PlayerState.is_assigning) {
        return;
    }

    PlayerArray assigned{};
    const size_t limit = players::get_max_number_of_players();
    for (size_t i = 0; i < count && assigned.get_count() < limit; i++) {
        if (controllers[i] != nullptr) {
            assigned.add_controller_player(controllers[i]);
        }
    }
    if (assigned.get_count() == 0) {
        assigned.add_keyboard_player();
    }

    PlayerState.players = assigned;

    // The same profile assignment commit_player_assignment performs, so that a
    // player's own remapping follows its pad into whichever slot it lands in.
    for (int i = 0; i < (int)PlayerState.players.get_count(); i++) {
        Player &player = PlayerState.players[i];
        if (player.controller != nullptr) {
            int cont_profile_index = profiles::get_controller_profile_index_from_sdl_controller(player.controller);
            if (cont_profile_index >= 0) {
                profiles::set_input_profile_for_player(i, cont_profile_index, InputDevice::Controller);
            }
        } else {
            profiles::set_input_profile_for_player(i, profiles::get_or_create_mp_keyboard_profile_index(i), InputDevice::Keyboard);
        }
    }
}

// playerassignment start"""


def patch(path, anchor, replacement, marker):
    if not path.exists():
        sys.exit(f"missing {path}\nRun: git submodule update --init --recursive")

    text = path.read_text(encoding="utf-8")

    if marker in text:
        print(f"  {path.name} already patched")
        return

    if anchor not in text:
        sys.exit(f"anchor not found in {path}; upstream has changed and this "
                 f"patch needs revisiting")

    path.write_text(text.replace(anchor, replacement, 1), encoding="utf-8")
    print(f"  {path.name} patched")


def main():
    patch(HEADER, HEADER_ANCHOR, HEADER_REPLACEMENT, "auto_assign_controllers")
    patch(SOURCE, SOURCE_ANCHOR, SOURCE_REPLACEMENT, "auto_assign_controllers")
    print("\nRebuild to pick it up.")


if __name__ == "__main__":
    main()
