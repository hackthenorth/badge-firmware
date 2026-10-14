// Regression checks use real app button handlers, LVGL and persisted config.
void test_app_working_memory()
{
    auto cycle = []() {
        enter(app_outrun_instance());
        active->on_tick();expect("OUTRUN");
        active->on_button({Button::A,ButtonEventKind::Pressed});
        for(int i=0;i<100;++i)active->on_tick();
        active->on_button({Button::A,ButtonEventKind::Released});
        refresh();
        assert(std::any_of(labels.begin(),labels.end(),[](const std::string &s){
            return s.find("SCORE ")!=std::string::npos;
        }));
        enter(app_sponsors_instance());
        for(int i=0;i<6;++i)active->on_tick();
        expect("Floor 1");press(Button::Start);press(Button::B);
        enter(app_etch_instance());
        active->on_button({Button::Right,ButtonEventKind::Pressed});
        for(int i=0;i<12;++i)active->on_tick();
        active->on_button({Button::Right,ButtonEventKind::Released});
        assert(lv_obj_get_child_count(lv_obj_get_child(root,0))>2);
        press(Button::B);
        enter(app_flashlight_instance());refresh();
    };
    cycle();cycle(); // Warm lazy host/runtime allocations before measuring.
    const size_t baseline=__sanitizer_get_current_allocated_bytes();
    for(int i=0;i<6;++i)cycle();
    assert(__sanitizer_get_current_allocated_bytes()<=baseline);
    for(App *app:{app_outrun_instance(),app_sponsors_instance(),app_etch_instance()}) {
        fail_app_allocation=true;
        enter(app);assert(!fail_app_allocation);
        expect("Not enough memory\nHOME for Apps");
        active->on_tick();press(Button::A);active->checkpoint_progress();
        enter(app_flashlight_instance());refresh();
    }
    assert(__sanitizer_get_current_allocated_bytes()<=baseline);
    cycle(); // An allocation failure must not poison the next visit.
    puts("Outrun, Sponsors and Etch: working memory released across visits; entry OOM recovers: PASS");
}

void test_games()
{
    test_app_working_memory();
    hal_config_set_int("sokoban", "level", 0);
    hal_config_set_int("sokoban", "best0", 1);
    enter(app_sokoban_instance());
    for (int i=0; i<2500; ++i) { press(Button::Down); press(Button::Up); }
    press(Button::Right); expect("Solved in 5001!  A: level 2"); shot("sokoban-over-par");
    assert(hal_config_get_int("sokoban", "best0", 0)==1);
    press(Button::B); press(Button::Right); expect("Solved in 5001!  A: level 2");
    press(Button::A); expect("Sokoban 2/12");

    enter(app_pong_instance());
    active->on_button({Button::Up,ButtonEventKind::Pressed});
    for(int i=0; i<10000 && !label(root,"YOU LOSE  A: again"); ++i) active->on_tick();
    expect("YOU LOSE  A: again"); shot("pong-loss");
    press(Button::A); expect("You 0 : 0 CPU   Wins 0"); shot("pong-court");
    // Field coordinates must be exact: no theme padding can clip the CPU paddle.
    auto *field=lv_obj_get_child(root,1);
    assert(lv_obj_get_style_pad_left(field,LV_PART_MAIN)==0 && lv_obj_get_style_pad_right(field,LV_PART_MAIN)==0);

    random_value=0;
    enter(app_dance_instance());
    expect("45s left  Misses 0/5  Hit in box");
    auto *dance_field=lv_obj_get_child(root,1);
    auto *dance_zone=lv_obj_get_child(dance_field,0);
    assert(lv_obj_get_height(dance_zone)==39);
    for(int i=0;i<20;++i)active->on_tick();
    auto *first_note=lv_obj_get_child(dance_field,1);
    assert(!lv_obj_has_flag(first_note,LV_OBJ_FLAG_HIDDEN));
    press(Button::Left);expect("45s left  Misses 1/5  Hit in box");
    assert(lv_obj_has_flag(first_note,LV_OBJ_FLAG_HIDDEN));
    for(int i=0;i<69;++i)active->on_tick();
    press(Button::Left);expect("44s left  Misses 0/5  Hit in box");
    shot("dance-hit-zone");
    enter(app_dance_instance());
    for(int i=0;i<5;++i)press(Button::Left);
    expect("GAME OVER - 5 misses. A: try again");shot("dance-loss");
    press(Button::A);expect("45s left  Misses 0/5  Hit in box");
    for(int i=0;i<500 && !label(root,"GAME OVER - 5 misses. A: try again");++i)active->on_tick();
    expect("GAME OVER - 5 misses. A: try again");
    random_value=73;

    enter(app_etch_instance());
    auto *pad=lv_obj_get_child(root,0);const auto dots=lv_obj_get_child_count(pad);
    press(Button::A);expect("Pen UP  A: draw  D-pad: move  B: clear");
    active->on_button({Button::Right,ButtonEventKind::Pressed});active->on_tick();
    active->on_button({Button::Right,ButtonEventKind::Released});
    assert(lv_obj_get_child_count(pad)==dots);
    press(Button::B);expect("Pen UP  A: draw  D-pad: move  B: clear");
    press(Button::A);expect("Pen DOWN  A: lift  D-pad: move  B: clear");shot("etch-pen");

    enter(app_hopper_instance());
    for(int slot=0;slot<5;++slot) {
        const int column=slot*3;
        for(int i=6;i>column;--i)press(Button::Left);
        for(int i=6;i<column;++i)press(Button::Right);
        for(int i=0;i<7;++i)press(Button::Up);
        assert(hal_config_get_int("hopper","levels_completed",0)==(slot==4?1:0));
    }
    shot("hopper-level-cleared");
    auto mines = []() {
        // Isolate the safe top-left square so the first flood cannot win.
        random_queue={1,0,0,1,1,1,2,0,3,0,4,0,5,0,6,0,7,0,8,0};
    };
    hal_config_set_int("mines","cleared",0);
    enter(app_mines_instance());mines();press(Button::A);
    for(int i=0;i<4;++i){press(Button::Up);press(Button::Left);}
    press(Button::Right);press(Button::A); // mine at 1,0
    assert(hal_config_get_int("mines","cleared",0)==0);
    press(Button::A);mines();press(Button::A);
    for(int i=0;i<4;++i){press(Button::Up);press(Button::Left);}
    press(Button::A); // final safe square, without advancing the clock
    assert(hal_config_get_int("mines","cleared",0)==1);
    assert(hal_config_get_int("mines","best_s",-1)==0);shot("mines-cleared");
    hal_config_set_int("2048","high_score",99999);hal_config_set_int("2048","max_tile",0);
    random_queue={0,1,0,1};enter(app_2048_instance());press(Button::Left);
    assert(hal_config_get_int("2048","max_tile",0)==4); // tile, not points
    lv_tick_inc(500);active->on_tick();lv_tick_inc(500);active->on_tick();shot("2048-tile");

    hal_config_set_int("flappy","high_score",0);random_value=48;
    enter(app_flappy_instance());press(Button::A);
    for(int i=0;i<170;++i){if(i>0&&i%23==0)press(Button::A);active->on_tick();}
    assert(hal_config_get_int("flappy","high_score",0)==0);
    press(Button::Start);expect("Paused\nStart or B to resume\nHome for Apps");
    assert(hal_config_get_int("flappy","high_score",0)>0);random_value=73;
    shot("flappy-paused-score");

    // Moving right, Up then Left in one movement interval must execute across
    // two steps. If Left is dropped, continuing Up hits the wall by step nine.
    enter(app_snake_instance());press(Button::Up);press(Button::Left);
    for(int i=0;i<72;++i)active->on_tick();
    assert(!label(root,"Game over\nA to restart"));shot("snake-turn-queue");

    assert(app_blackjack_instance()->access()==AppAccess::Public);
    enter(app_blackjack_instance());expect("Ties win. Dealer stands on 17.");shot("blackjack-deal");
    for(int i=0;i<100;++i){press(Button::B);refresh();press(Button::A);refresh();}
    press(Button::B);shot("blackjack-result");
    assert(hal_config_get_int("blackjack","wins",0)+hal_config_get_int("blackjack","losses",0)>0);
    assert(app_flashlight_instance()->access()==AppAccess::Internal);
    enter(app_flashlight_instance());shot("flashlight-diagnostic");
    enter(app_fortune_instance());expect("The crumbs know all.");shot("fortune-cookie");
    enter(app_reaction_instance());expect("Ready?");shot("reaction-ready");
    enter(app_hardware_physics_instance());shot("hardware");
    enter(app_ktnh_instance());expect("A: OPEN READY ROOM");press(Button::A);
    expect("START: EVERYBODY READY   B: BACK");press(Button::Start);expect("START: modules");shot("ktnh-module-hint");
    press(Button::Start);expect("MODULES");press(Button::Start);expect("START: modules");
    puts("Games: long Sokoban solve/undo, Pong loss/court, Dance hit recovery/miss limit, pen toggle, Hopper levels, Flappy pause save, Snake turn queue, Blackjack, diagnostic flashlight and KTNH module hint: PASS");
}
