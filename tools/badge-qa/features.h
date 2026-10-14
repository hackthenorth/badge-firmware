// Included in the host harness to exercise real apps and file-backed wallets.
void expect_pal_artwork(){
 refresh();auto *buf=lv_snapshot_take(root,LV_COLOR_FORMAT_RGB888);assert(buf);
 for(int cell=0;cell<9;++cell){
  int visible=0;
  for(int y=48+(cell/3)*45;y<79+(cell/3)*45;++y)
   for(int x=45+(cell%3)*99;x<76+(cell%3)*99;++x){
    auto *p=buf->data+y*buf->header.stride+x*3;
    if(p[0]>32||p[1]>32||p[2]>32)++visible;
   }
  assert(visible>40); // fail if an image silently disappears during decoding
 }
 lv_draw_buf_destroy(buf);
}
void test_features(){
 identity.provisioned = true;
 identity.role = ROLE_HACKER; // hacker; organizers intentionally have every PAL
 strcpy(identity.badge_id,"qa-badge");strcpy(identity.display_name,"Badge QA");
 std::filesystem::remove("wallet.json");solana_wallet_init();
 radio_starts=0;
 enter(app_blindbox_instance());expect("Solana Scavenger Hunt");
 expect("A: start hunting   Start: help anytime");assert(radio_starts==0);shot("hunt-intro");
 press(Button::A);expect("Bring your badge close to a\nSolana Scavenger Hunt box");
 assert(radio_starts>0&&radio_cb);assert(hal_config_get_int("blindbox","intro_seen",0)==1);
 press(Button::A);expect("0/9 pals unlocked");expect("QR code only");assert(!radio_cb);shot("pals-locked");expect_pal_artwork();
 press(Button::Down);expect("Ginny - Locked In (locked)");press(Button::A);
 expect("Find a Solana box to unlock this pal");assert(solana_wallet_equipped_pal()==0);
 press(Button::B);expect("Bring your badge close to a\nSolana Scavenger Hunt box");
 press(Button::Start);expect("A: start hunting   Start: help anytime");press(Button::B);
 enter(app_blindbox_instance());expect("Bring your badge close to a\nSolana Scavenger Hunt box");
 const char *items[]={"1","6","7","9","future-pal"};solana_wallet_replace_items(items,5);
 press(Button::A);expect("4/9 pals unlocked");press(Button::Down);press(Button::Right);press(Button::Right);
 expect("Ginny - Heart");shot("pals-partial");expect_pal_artwork();press(Button::A);assert(solana_wallet_equipped_pal()==6);
 solana_wallet_init();assert(solana_wallet_equipped_pal()==6&&solana_wallet_item_count()==5);
 enter(app_badge_id_instance());expect("A Customize   Start Details   Home Apps");shot("badge-pal-qr");
 press(Button::A);expect("Ginny - Heart");press(Button::Down);expect("Patch - Curious");
 press(Button::Down);expect("Vinyl - Sparkle");press(Button::Down);expect("QR code only");
 press(Button::A);assert(solana_wallet_equipped_pal()==0);expect("A Customize   Start Details   Home Apps");
 press(Button::Start);expect("Start/Up/B QR   Home Apps");press(Button::B);expect("A Customize   Start Details   Home Apps");
 hal_config_set_int("ripple","depth",255);
 enter(app_badge_id_instance());expect("A Customize   Start Details   Home Apps");
 auto *depth=label(root,"255");assert(depth&&lv_obj_get_parent(depth)==root);
 assert(lv_obj_get_style_bg_opa(depth,LV_PART_MAIN)==LV_OPA_TRANSP);shot("ripple-grey");
 hal_config_set_int("ripple","depth",-1);
 identity.role=ROLE_ORGANIZER; // organizer override, without fake inventory writes
 press(Button::A);expect("9/9 pals unlocked");shot("pals-all");identity.role=ROLE_HACKER;
 radio_fail=true;enter(app_blindbox_instance());expect("Radio wouldn't start");
 press(Button::B);expect("4/9 pals unlocked");press(Button::B);press(Button::Start);expect("A: start hunting   Start: help anytime");radio_fail=false;
 // Failed selection saves leave both the current choice and disk unchanged.
 std::filesystem::create_directory("wallet.json.tmp");assert(!solana_wallet_set_equipped_pal(1));assert(solana_wallet_equipped_pal()==0);std::filesystem::remove("wallet.json.tmp");
 enter(app_progress_instance());assert(active->access()==AppAccess::Default);shot("progress-ring");
 enter(app_breakout_instance());expect("BRICKS  0     BEST  0");shot("brick-breaker");
 enter(app_dice_instance());assert(active->access()==AppAccess::Default);
 random_value=19;press(Button::A);for(int i=0;i<10;++i)active->on_tick();expect("20");assert(leds[0][1]==66);
 random_value=73;time_us+=1600000;hal_lights_tick();
 enter(app_tictactoe_instance());
 for(auto b:{Button::Up,Button::Left,Button::A,Button::Down,Button::A,Button::Up,Button::Right,Button::A,Button::Down,Button::A,Button::Up,Button::Right,Button::A})press(b);
 assert(hal_config_get_int("tictactoe","x_wins",0)==1&&leds[0][1]==66);
 time_us+=1600000;hal_lights_tick();
 puts("Hunt intro/help, grouped PAL inventory, QR customization, persistence, failure handling, Progress and game celebrations: PASS");
}
