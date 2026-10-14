// Exercise real Connect button dispatch, radio receive queues, UI and storage.
void test_connect(){
 strcpy(identity.badge_id,"connect-test-owner");
 const uint8_t peer_mac[]={2,3,4,5,6,7};
 ButtonBump::Frame peer;
 peer.nonce=987;
 strcpy(peer.card.badge_id,"button-peer");strcpy(peer.card.display_name,"Button Friend");
 strcpy(peer.card.net_email,"button@example.test");
 strcpy(peer.card.net_linkedin,"button-friend");
 strcpy(peer.card.net_discord,"button_friend");
 auto receive=[&](int64_t at,int8_t rssi=-40){
  time_us=at;uint8_t bytes[BUMP_MAX_FRAME];auto size=ButtonBump::encode(bytes,sizeof(bytes),peer);
  assert(size&&radio_cb);radio_cb(peer_mac,rssi,bytes,size);active->on_tick();
 };
 auto transmitted=[](){
  ButtonBump::Frame frame;
  assert(ButtonBump::decode(last_transmission.data(),last_transmission.size(),frame));
  return frame;
 };
 auto start=[&](int64_t at){
  time_us=at;random_value++;peer.ack_nonce=0;
  enter(app_connect_instance());assert(!advertising&&!radio_enabled&&!radio_cb);
  press(Button::A);assert(advertising&&radio_enabled&&radio_cb);
  return transmitted().nonce;
 };

 provisioned=false;last_transmission.clear();enter(app_connect_instance());press(Button::A);
 assert(last_transmission.empty()&&!advertising);provisioned=true;
 radio_fail=true;enter(app_connect_instance());press(Button::A);
 assert(last_transmission.empty()&&!advertising);radio_fail=false;
 enter(app_connect_instance());radio_fail=true;press(Button::A);
 assert(last_transmission.empty()&&!radio_enabled&&!advertising);radio_fail=false;

 auto nonce=start(1000000);
 assert(scan_interval_ms==100&&scan_window_ms==100);
 expect("CONNECT BADGES");expect("PAIRING...");
 expect("Both press or hold A\nStay within 10 cm");
 expect("Hold A for up to 2 seconds\nB/Start  Contacts     Home  Apps");shot("connect-ready");
 // Weak/unknown RSSI cannot confirm a button press. The tighter proximity
 // filter rejects -65 dBm but accepts the exact -64 dBm boundary.
 receive(1010000,-65);receive(1020000,127);assert(transmitted().ack_nonce==0);
 receive(1030000,-64);assert(transmitted().ack_nonce==peer.nonce);
 // Physical-bump packets cannot confirm a button press.
 uint8_t legacy[BUMP_MAX_FRAME];auto size=bump_encode(legacy,sizeof(legacy),peer.card,nullptr);
 radio_cb(peer_mac,-40,legacy,size);active->on_tick();assert(contacts_store_count()==0);
 time_us=1500000;random_value++;press(Button::A);assert(transmitted().nonce==nonce);
 receive(1999000);assert(transmitted().ack_nonce==peer.nonce&&contacts_store_count()==0);
 peer.ack_nonce=nonce;
 receive(2010000);assert(contacts_store_count()==1);expect("Contacts: 1");
 for(int i=0;i<12;++i)active->on_tick();expect("SYNCED");shot("connect-synced");
 receive(2020000);assert(contacts_store_count()==1); // Duplicate ACK, same attempt.
 time_us=2500001;active->on_tick();assert(!advertising&&!radio_enabled&&!radio_cb);

 // A second exchange with the same badge updates its card but neither grows
 // the unique count nor hides the fact that this person was already saved.
 time_us=3000000;random_value++;peer.ack_nonce=0;enter(app_connect_instance());
 active->on_button({Button::A,ButtonEventKind::Pressed});nonce=transmitted().nonce;
 time_us=3500000;active->on_button({Button::A,ButtonEventKind::Released});
 assert(advertising&&radio_enabled&&transmitted().nonce==nonce);
 receive(3999000);peer.ack_nonce=nonce;receive(4010000);
 assert(contacts_store_count()==1);expect("Contacts: 1");
 for(int i=0;i<12;++i)active->on_tick();expect("Already connected");
 // Dimmed yellow duplicate: logical (255,204,0) >> 1 = (127,102,0), which the
 // HAL's squared 26%-capped curve lands at physical (16,11,0).
 for(auto &led:leds)assert(led[0]==16&&led[1]==11&&led[2]==0);
 assert(lv_color_eq(lv_obj_get_style_text_color(label(root,"Already connected"),LV_PART_MAIN),lv_color_hex(0xffcc00)));
 expect("Already connected: Button Friend");shot("connect-already");
 time_us=4500001;active->on_tick();assert(!advertising&&!radio_enabled&&!radio_cb);

 // A matching ACK first seen just after the one-second gate proves that the
 // peer heard us during it, so BLE transport delay does not lose an edge tap.
 nonce=start(5000000);
 peer.ack_nonce=nonce;receive(6200000);
 assert(contacts_store_count()==1);expect("Contacts: 1");
 time_us=6500001;active->on_tick();assert(!advertising&&!radio_enabled&&!radio_cb);

 // A new, unacknowledged peer first seen after one second is still too late.
 nonce=start(7000000);
 strcpy(peer.card.badge_id,"late-peer");peer.ack_nonce=0;
 receive(8001000);assert(contacts_store_count()==1&&!advertising);
 time_us=8500001;active->on_tick();assert(!advertising&&!radio_enabled&&!radio_cb);

 // A packet received before A must not become fresh just because its queue
 // is drained afterward. A stale nonce also cannot confirm the new attempt.
 time_us=9000000;enter(app_connect_instance());
 assert(!radio_enabled&&!radio_cb);
 active->on_tick();accel_sample.z_mg=2000;active->on_tick();accel_sample.z_mg=1000;
 peer.ack_nonce=0;size=ButtonBump::encode(legacy,sizeof(legacy),peer);
 radio_cb(peer_mac,-40,legacy,size);
 time_us=9010000;random_value++;press(Button::A);active->on_tick();
 assert(transmitted().ack_nonce==0);
 peer.ack_nonce=nonce;receive(9020000);assert(contacts_store_count()==1);
 time_us=10510001;active->on_tick();assert(!advertising); // Unmatched TX expires.

 press(Button::B);assert(!advertising&&!radio_cb);shot("connect-contacts");
 refresh();for(auto &text:labels)assert(text.find("button-peer")==std::string::npos);
 auto before=last_transmission;press(Button::A);assert(last_transmission==before&&!advertising);
 expect("Contact 1/1");expect("Button Friend");expect("> Email  button@example.test");
 refresh();for(auto &text:labels)assert(text.find("button-peer")==std::string::npos);
 press(Button::Down);expect("> Phone  Not shared");
 press(Button::Down);expect("> LinkedIn  button-friend");press(Button::A);
 expect("LinkedIn - Button Friend");expect("button-friend");
 refresh();for(auto &text:labels)assert(text.find("button-peer")==std::string::npos);
 press(Button::B);expect("Contact 1/1");press(Button::B);expect("Contacts (1)");
 press(Button::B);assert(!radio_enabled&&!radio_cb&&!advertising);

 // Smaller movement and the exact 400 mg boundary stay quiet. A deliberate
 // combined-axis impact still triggers even when each axis is below 400 mg.
 accel_sample={0,0,1000};active->on_tick();
 accel_sample={220,220,1000};active->on_tick();
 assert(!radio_enabled&&!advertising&&!radio_cb);
 accel_sample={0,0,1000};active->on_tick();
 accel_sample={400,0,1000};active->on_tick();
 assert(!radio_enabled&&!advertising&&!radio_cb);
 accel_sample={0,0,1000};active->on_tick();
 accel_sample={300,300,1000};active->on_tick();
 assert(advertising&&memcmp(last_transmission.data(),BUMP_MAGIC,4)==0);
 accel_sample={0,0,1000};
 strcpy(peer.card.badge_id,"physical-peer");
 strcpy(peer.card.display_name,"Physical Friend");
 strcpy(peer.card.net_email,"physical@example.test");
 peer.card.net_linkedin[0]='\0';
 size=bump_encode(legacy,sizeof(legacy),peer.card,nullptr);
 radio_cb(peer_mac,-65,legacy,size);active->on_tick();assert(contacts_store_count()==1);
 radio_cb(peer_mac,127,legacy,size);active->on_tick();assert(contacts_store_count()==1);
 radio_cb(peer_mac,-64,legacy,size);active->on_tick();assert(contacts_store_count()==2);
 for(int i=0;i<60;++i){time_us+=20000;active->on_tick();}assert(!radio_enabled&&!advertising&&!radio_cb);
 press(Button::B);expect("Contacts (2)");press(Button::A);expect("Contact 1/2");
 expect("Button Friend");press(Button::Right);expect("Contact 2/2");expect("Physical Friend");
 press(Button::Left);expect("Contact 1/2");expect("Button Friend");
 shot("contact-links-detail");
 refresh();for(auto &text:labels){
  assert(text.find("button-peer")==std::string::npos);
  assert(text.find("physical-peer")==std::string::npos);
  assert(text.find(".cfg")==std::string::npos);
 }
 press(Button::Down);expect("> Phone  Not shared");
 press(Button::Down);press(Button::A);expect("LinkedIn - Button Friend");
 shot("contact-link-qr");press(Button::Right);
 expect("LinkedIn - Physical Friend");expect("Physical Friend did not share\nLinkedIn");
 shot("contact-link-missing");press(Button::Left);expect("LinkedIn - Button Friend");
 expect("button-friend");press(Button::B);press(Button::B);press(Button::Start);
 assert(!radio_enabled&&!advertising&&!radio_cb);
 enter(app_mood_instance());assert(!advertising&&!radio_cb);
 // Holding A extends discovery, but release and the two-second cap remain
 // absolute. A failed attempt reports red feedback for two real seconds.
 auto hold_started=time_us+2000000;time_us=hold_started;enter(app_connect_instance());
 active->on_button({Button::A,ButtonEventKind::Pressed});
 time_us=hold_started+1500000;active->on_tick();assert(advertising&&radio_enabled);
 active->on_button({Button::A,ButtonEventKind::Released});
 time_us++;active->on_tick();assert(!advertising&&radio_enabled);
 time_us=hold_started+2000001;active->on_tick();assert(!radio_enabled&&!radio_cb);
 expect("No badge found,\ntry again");
 auto failed_at=time_us;time_us=failed_at+1999999;active->on_tick();expect("No badge found,\ntry again");
 time_us++;active->on_tick();expect("Bump badges or\nPress A");
 hold_started=time_us;active->on_button({Button::A,ButtonEventKind::Pressed});
 time_us=hold_started+1999999;active->on_tick();assert(advertising);
 time_us=hold_started+2000001;active->on_tick();assert(!advertising&&radio_enabled);
 time_us=hold_started+2500001;active->on_tick();assert(!advertising&&!radio_enabled);
 enter(app_mood_instance());
 puts("Connect: 10 cm guidance, tighter proximity, private contact IDs, navigable links/QRs, duplicate contacts, stale frames and combined-axis bumps: PASS");
}
