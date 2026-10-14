#pragma once

class App;
App *app_launcher_instance();
App *app_badge_id_instance();
App *app_onboarding_instance();
App *app_progress_instance();
App *app_schedule_instance();
App *app_conversation_instance();
App *app_museum_instance();
App *app_sponsors_instance();
App *app_workshops_instance();
App *app_rules_instance();

// True when the user has not completed onboarding on this firmware build.
int  badge_onboarding_required();

// Mark onboarding as not-yet-done, so the next boot replays the tutorial.
// Provisioning calls this: a freshly registered badge is about to be handed to
// a person who has never used one.
void badge_onboarding_reset();

App *app_accel_instance();
App *app_lights_instance();
App *app_nfc_instance();
App *app_buttons_instance();
App *app_lightshow_instance();
App *app_mood_instance();
App *app_kitchen_instance();
App *app_connect_instance();
App *app_share_instance();

// v2 batch
App *app_level_instance();
App *app_pomodoro_instance();
App *app_flashlight_instance();
App *app_67_instance();
App *app_lanyards_instance();
App *app_directory_instance();
App *app_diagnostics_instance();
App *app_meminfo_instance();
App *app_power_instance();
App *app_files_instance();
App *app_socials_instance();
App *app_dice_instance();
App *app_eightball_instance();
App *app_maze_instance();
App *app_snake_instance();
App *app_tetris_instance();
App *app_2048_instance();
App *app_breakout_instance();
App *app_tictactoe_instance();
App *app_chess_instance();
App *app_simon_instance();
App *app_flappy_instance();
App *app_bubble_wrap_instance();
App *app_ktnh_instance();
App *app_hardware_physics_instance();
App *app_scanner_instance();
App *app_test_instance();
App *app_blindbox_instance();
App *app_sync_instance();

// v3 batch: reaction, party games, toys, dance
App *app_reaction_instance();
App *app_whack_instance();
App *app_pong_instance();
App *app_slots_instance();
App *app_blackjack_instance();
App *app_hangman_instance();
App *app_mines_instance();
App *app_fortune_instance();
App *app_etch_instance();
App *app_kaleido_instance();
App *app_lava_instance();
App *app_dance_instance();
// Arcade batch: sokoban, hopper, jetpack, invaders, blocks, outrun.
App *app_sokoban_instance();
App *app_hopper_instance();
App *app_jetpack_instance();
App *app_invaders_instance();
App *app_blocks_instance();
App *app_outrun_instance();
App *app_teardown_instance();
