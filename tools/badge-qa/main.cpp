#include "app.h"
#include "app_registry.h"
#include "app_reboot.h"
#include "app_launcher.h"
#include "app_scanner.h"
#include "app_sponsors.h"
#include "app_workshops.h"
#include "hal_config.h"
#include "button_bump.h"
#include "mp_session.h"
#include "share_xfer.h"
#include "app_share_proto.h"
#include "contacts_store.h"
#include "hal_gesture.h"
#include "hal_identity.h"
#include "hal_lights.h"
#include "hal_nfc.h"
#include "hal_radio.h"
#include "pal_picker.h"
#include "solana_wallet.h"
#include "launcher_slugs.h"
#include "led_strip_rmt.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <deque>
#include <new>
#include <sanitizer/allocator_interface.h>

// Fail the next explicit nonthrowing allocation to exercise app entry OOM.
static bool fail_app_allocation = false;
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
 if(fail_app_allocation){fail_app_allocation=false;return nullptr;}
 return ::operator new(size);
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
 if(fail_app_allocation){fail_app_allocation=false;return nullptr;}
 return ::operator new[](size);
}

static const char *config_dir = BADGE_QA_DIR "/config";
static lv_obj_t *root;
static App *active;
static App *launched;
static App *shell;
static BadgeIdentity identity;
static bool provisioned = true;
static bool shake = false;
static int leds[6][3];
static int pending_leds[6][3];
static int64_t time_us;
static std::vector<App *> registry;
static std::vector<std::string> labels;
extern "C" const lv_image_dsc_t icon_eightball, icon_flashlight, icon_lights,
 icon_breakout, icon_maze, icon_tictactoe, icon_flappy, icon_socials, icon_sponsors;

bool hal_fs_mkdir_p(const char *path){std::filesystem::create_directories(path);return true;}
const BadgeIdentity *hal_identity_get(){return &identity;}
bool hal_identity_is_provisioned(){return provisioned;}
const char *hal_identity_role_name(uint8_t){return "Hacker";}
bool gesture_poll_shake(){bool result=shake;shake=false;return result;}
AccelSample gesture_last_sample(){return {0,0,1000};}
int64_t esp_timer_get_time(){return time_us;}
esp_err_t led_strip_new_rmt_device(const led_strip_config_t *config,const led_strip_rmt_config_t*,led_strip_handle_t *strip){assert(config->max_leds==LED_COUNT);*strip=pending_leds;return ESP_OK;}
esp_err_t led_strip_set_pixel(led_strip_handle_t,uint32_t n,uint32_t r,uint32_t g,uint32_t b){assert(n<LED_COUNT);pending_leds[n][0]=r;pending_leds[n][1]=g;pending_leds[n][2]=b;return ESP_OK;}
esp_err_t led_strip_clear(led_strip_handle_t){memset(pending_leds,0,sizeof(pending_leds));memset(leds,0,sizeof(leds));return ESP_OK;}
esp_err_t led_strip_refresh(led_strip_handle_t){memcpy(leds,pending_leds,sizeof(leds));return ESP_OK;}
esp_err_t hal_nfc_enable(){return ESP_OK;}
void hal_nfc_field_down(){}
bool app_reboot_take_launcher_focus(char*,size_t){return false;}
void hal_nfc_clear_last_card(){}
bool hal_nfc_last_card(NfcCard*){return false;}
esp_err_t hal_nfc_read_text(char*,size_t){return ESP_FAIL;}
static uint32_t random_value = 73;
static std::deque<uint32_t> random_queue;
uint32_t esp_random(){if(random_queue.empty())return random_value;auto value=random_queue.front();random_queue.pop_front();return value;}
void esp_fill_random(void *p,size_t n){memset(p,0x42,n);}
uint32_t hal_identity_revision(){return 0;}
void hal_identity_role_default_color(uint8_t,uint8_t out[3]){out[0]=100;out[1]=160;out[2]=200;}
static int radio_starts=0;
static bool radio_fail=false;
static bool radio_enabled=false;
static RadioRecvCb radio_cb;
static bool advertising=false;
static uint32_t scan_interval_ms=0,scan_window_ms=0;
static std::vector<uint8_t> last_transmission;
static AccelSample accel_sample{0,0,1000};
esp_err_t hal_accel_set_output_rate(AccelOutputRate){return ESP_OK;}
esp_err_t hal_accel_read(AccelSample *out){*out=accel_sample;return ESP_OK;}
esp_err_t hal_radio_enable(){++radio_starts;radio_enabled=!radio_fail;return radio_enabled?ESP_OK:ESP_FAIL;}
void hal_radio_set_recv_cb(RadioRecvCb cb){radio_cb=cb;}
void hal_radio_local_mac(uint8_t out[6]){memset(out,0x42,6);}
esp_err_t hal_radio_send(const void *data,size_t size){if(!radio_enabled)return ESP_FAIL;auto *p=static_cast<const uint8_t*>(data);last_transmission.assign(p,p+size);return ESP_OK;}
void hal_radio_reset_timings(){}
void hal_sleep_wake_lock(bool){}
void hal_radio_disable(){radio_enabled=false;advertising=false;}
esp_err_t hal_radio_adv_pause(){advertising=false;return ESP_OK;}
esp_err_t hal_radio_adv_resume(){advertising=true;return ESP_OK;}
esp_err_t hal_radio_set_adv_interval_ms(uint32_t,uint32_t){return ESP_OK;}
esp_err_t hal_radio_set_scan_duty_ms(uint32_t interval,uint32_t window){scan_interval_ms=interval;scan_window_ms=window;return ESP_OK;}
size_t app_registry_count(){return registry.size();}
App *app_registry_get(size_t n){return n<registry.size()?registry[n]:nullptr;}
App *app_registry_find_by_slug(const char *s){for(auto *a:registry)if(strcmp(a->slug(),s)==0)return a;return nullptr;}
void app_registry_launch(App *a){launched=a;}
void app_registry_set_launcher(App *a){shell=a;}

struct Toy:App {
 const char *id; AppAccess tier; const lv_image_dsc_t *image; bool outline; bool mask;
 Toy(const char *s,AppAccess t=AppAccess::Default,const lv_image_dsc_t *i=nullptr,bool o=false,bool m=false):id(s),tier(t),image(i),outline(o),mask(m){}
 const char *name()const override{return id;}
 const char *icon()const override{return id;}
 const char *slug()const override{return id;}
 AppAccess access()const override{return tier;}
 const lv_image_dsc_t *icon_image()const override{return image;}
 bool icon_needs_outline()const override{return outline;}
 bool icon_is_mask()const override{return mask;}
 void on_enter(lv_obj_t*)override{}
};
Toy badge("badgeid"),connect("connect"),share("share"),hunt("blindbox"),
 sponsors("sponsors",AppAccess::Default,&icon_sponsors),workshops("workshops"),
 socials("socials",AppAccess::Default,&icon_socials,false,true),schedule("schedule"),museum("museum"),
 snake("snake",AppAccess::Default),flashlight("flashlight",AppAccess::Internal,&icon_flashlight,true);
App *app_test_instance(){return &flashlight;}
void app_registry_return_to_launcher(){launched=shell;}
bool app_reboot_into_app(App *){return false;}

void collect(lv_obj_t *o){
 if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
 if(lv_obj_check_type(o,&lv_label_class)){
  auto *text=lv_label_get_text(o);labels.emplace_back(text);
  lv_area_t area;lv_obj_get_coords(o,&area);
  if(area.x1<0||area.y1<0||area.x2>=320||area.y2>=240){fprintf(stderr,"offscreen [%d,%d..%d,%d] %s\n",area.x1,area.y1,area.x2,area.y2,text);assert(false);}
 }
 for(uint32_t i=0;i<lv_obj_get_child_count(o);++i)collect(lv_obj_get_child(o,i));
}
void refresh(){lv_obj_update_layout(root);labels.clear();collect(root);}
void expect(const char *text){refresh();if(std::find(labels.begin(),labels.end(),text)==labels.end()){for(auto&s:labels)fprintf(stderr,"%s\n",s.c_str());fprintf(stderr,"Missing: %s\n",text);assert(false);}}
lv_obj_t *label(lv_obj_t *o,const char *text){if(lv_obj_check_type(o,&lv_label_class)&&strcmp(lv_label_get_text(o),text)==0)return o;for(uint32_t i=0;i<lv_obj_get_child_count(o);++i)if(auto *r=label(lv_obj_get_child(o,i),text))return r;return nullptr;}
void enter(App *app){if(active)active->on_exit();lv_obj_clean(root);active=app;active->on_enter(root);}
void press(Button b){active->on_button({b,ButtonEventKind::Pressed});active->on_button({b,ButtonEventKind::Released});}
void shot(const char *name){
 refresh();auto *buf=lv_snapshot_take(root,LV_COLOR_FORMAT_RGB888);assert(buf);
 auto path=std::string(BADGE_QA_DIR "/")+name+".ppm";
 FILE *f=fopen(path.c_str(),"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
 for(int y=0;y<240;++y)for(int x=0;x<320;++x){auto *p=buf->data+y*buf->header.stride+x*3;unsigned char rgb[]={p[2],p[1],p[0]};fwrite(rgb,1,3,f);}
 fclose(f);lv_draw_buf_destroy(buf);
}
std::string order(){std::string s;for(size_t i=0;i<launcher_slugs_count();++i){if(i)s+=",";s+=launcher_slugs_at(i);}return s;}
void reset_config(){std::filesystem::remove_all(config_dir);assert(hal_config_init()==ESP_OK);}
void test_lights(){
 int previous=-1;
 for(int value=0;value<=255;++value){
  hal_lights_set(0,value,value,value);hal_lights_show();
  assert(leds[0][0]>=previous&&leds[0][0]<=66);
  assert(leds[0][0]==leds[0][1]&&leds[0][1]==leds[0][2]);previous=leds[0][0];
 }
 assert(leds[0][0]==66);
 hal_lights_set(0,0,128,192);hal_lights_show();assert(leds[0][0]==0&&leds[0][1]==17&&leds[0][2]==37);
 hal_lights_flash_success();
 for(int i=0;i<6;++i)assert(leds[i][0]==0&&leds[i][1]==66&&leds[i][2]==0);
 hal_lights_set(0,255,0,192);hal_lights_show();assert(leds[0][0]==0&&leds[0][1]==66);
 for(int phase=1;phase<6;++phase){time_us=phase*250000;hal_lights_tick();assert(leds[0][1]==(phase%2?0:66));}
 time_us=1500000;hal_lights_tick();assert(leds[0][0]==66&&leds[0][1]==0&&leds[0][2]==37);
 hal_lights_clear();hal_lights_show();for(auto &led:leds)for(int channel:led)assert(channel==0);
 puts("Production LED HAL: 66/255 maximum, gamma curve, success pulses and frame restore: PASS");
}
#include "features.h"
#include "connect.h"
#include "games.h"
#include "pairing.h"

int main(int argc,char **argv){
 hal_lights_init();test_lights();
 lv_init();auto *d=lv_display_create(320,240);lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
 static uint8_t pixels[320*240*4];lv_display_set_buffers(d,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_FULL);
 lv_display_set_flush_cb(d,[](lv_display_t *display,const lv_area_t*,uint8_t*){lv_display_flush_ready(display);});
 root=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(root);lv_obj_set_size(root,320,240);
 lv_obj_set_style_bg_color(root,lv_color_hex(BadgeTheme::BACKGROUND),0);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
 if(argc==2&&strcmp(argv[1],"reboot")==0){
  identity.role_color[0]=64;identity.role_color[1]=12;
  assert(!badge_onboarding_required());enter(app_badge_id_instance());
  for(auto &led:leds)for(int channel:led)assert(channel==0);
  enter(app_onboarding_instance());expect("Any button to start");expect("1/11");
  for(auto &led:leds)for(int channel:led)assert(channel==0);
  press(Button::A);expect("2/11");
  for(auto &led:leds)assert(led[0]==37&&led[1]==1&&led[2]==0);
  puts("Separate process restart: QR and welcome stay dark; onboarding page 2 lights all six role LEDs: PASS");
  enter(app_bubble_wrap_instance());expect("ALL-TIME POPPED: 25");puts("Separate process reload: 25 all-time pops preserved");return 0;
 }
 reset_config();
 test_features();
 reset_config();
 strcpy(identity.display_name,"Alex");identity.role_color[0]=64;identity.role_color[1]=12;
 Toy unlocked("eightball",AppAccess::Public);
 Toy sync("sync");
 registry={&badge,&connect,app_scanner_instance(),&sync,&share,&hunt,&sponsors,&workshops,app_mood_instance(),&socials,&schedule,app_rules_instance(),&museum,app_progress_instance(),&snake,app_dice_instance(),&unlocked,app_blackjack_instance(),&flashlight};
 assert(app_registry_find_by_slug("blackjack")==app_blackjack_instance());
 assert(app_blackjack_instance()->access()==AppAccess::Public);
 const std::string prefix="badgeid,connect,scanner,sync,share,blindbox,sponsors,workshops,mood,socials,schedule,rules,museum";
 const std::string expected=prefix+",progress,snake,dice";
 launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==expected);
 launcher_slugs_move("badgeid",5);launcher_slugs_move("scanner",0);auto custom=order();launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==custom);
 for(auto mode:{LauncherAppMode::DefaultsOnly,LauncherAppMode::PublicApps,LauncherAppMode::PublicAndInternal}){
  launcher_slugs_set_mode(mode);assert(order().rfind(expected,0)==0);
  assert(launcher_slugs_contains("blackjack")== (mode!=LauncherAppMode::DefaultsOnly));
 }
 reset_config();launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==expected);
 for(int version:{8,9}) for(const char *tail:{"",",eightball,flashlight"}){
  std::string previous=version==9?"badgeid,connect,scanner,share":"badgeid,scanner,connect,share";
  previous+=",blindbox,sponsors,workshops,mood,socials,schedule,museum,rules";
  hal_config_set_string("system","launcher_order",(previous+tail).c_str());
  hal_config_set_int("system","default_apps_version",version);launcher_slugs_load();launcher_slugs_apply_defaults();
  const std::string migrated_tail=*tail ? ",eightball" : "";
  assert(order()==prefix+migrated_tail+",progress,snake,dice");
  auto once=order();launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==once);
 }
 for(const char *old:{"share,badgeid,scanner,connect,blindbox,sponsors,workshops,mood,socials,schedule,museum,rules", "badgeid,scanner,connect,blindbox,sponsors,workshops,mood,socials,schedule,museum,rules"}) {
  hal_config_set_string("system","launcher_order",old);hal_config_set_int("system","default_apps_version",8);
  launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==expected);
 }
 launcher_slugs_move("scanner",0);custom=order();hal_config_set_int("system","default_apps_version",9);launcher_slugs_load();launcher_slugs_apply_defaults();
 auto without_sync=[](std::string value){auto pos=value.find(",sync");if(pos!=std::string::npos)value.erase(pos,5);return value;};
 assert(without_sync(order())==without_sync(custom));assert(order().rfind("scanner,sync,",0)==0);
 // Upgrade the version-11 Museum placement, preserving custom app order.
 launcher_slugs_move("sync",12);custom=order();hal_config_set_int("system","default_apps_version",11);
 launcher_slugs_load();launcher_slugs_apply_defaults();
 assert(without_sync(order())==without_sync(custom));assert(order().rfind("scanner,sync,",0)==0);
 launcher_slugs_move("sync",12);custom=order();launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==custom);
 for(const char *mode:{"public","all"}){
  reset_config();hal_config_set_string("system","launcher_order",(expected+",eightball").c_str());
  hal_config_set_string("system","app_mode",mode);hal_config_set_int("system","default_apps_version",12);
  launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==expected+",eightball,blackjack");
  auto once=order();launcher_slugs_load();launcher_slugs_apply_defaults();assert(order()==once);
 }
 reset_config();launcher_slugs_load();launcher_slugs_apply_defaults();
 puts("Launcher defaults, reset, migration, custom order, Blackjack public mode and unlocked tail: PASS");

 for(auto b:{Button::Up,Button::Down,Button::Left,Button::Right,Button::Home,Button::Start,Button::B,Button::A}){
  enter(app_onboarding_instance());expect("Any button to start");
  for(auto &led:leds)for(int channel:led)assert(channel==0);
  active->on_button({b,ButtonEventKind::Released});expect("1/11");
  press(Button::Aux1);expect("1/11");press(b);expect("2/11");
  for(auto &led:leds)assert(led[0]==37&&led[1]==1&&led[2]==0);
 }
 enter(app_onboarding_instance());expect("Any button to start");shot("onboarding-welcome");
 press(Button::A);expect("2/11");for(auto &led:leds)assert(led[0]==37&&led[1]==1&&led[2]==0);
 press(Button::B);expect("1/11");for(auto &led:leds)for(int channel:led)assert(channel==0);press(Button::A);press(Button::A);expect("Try every button");
 press(Button::B);expect("Try every button");press(Button::B);expect("2/11");press(Button::A);expect("Try every button");
 assert(!label(root,"SideSwitch"));int button_leds_before[6][3];memcpy(button_leds_before,leds,sizeof(leds));
 active->on_button({Button::Aux1,ButtonEventKind::Pressed});active->on_button({Button::Aux1,ButtonEventKind::Released});
 expect("Try every button   B: back");assert(memcmp(button_leds_before,leds,sizeof(leds))==0);
 for(auto b:{Button::Up,Button::Down,Button::Left,Button::Right,Button::Home,Button::Start,Button::A})press(b);
 expect("A: next   B: back");assert(leds[5][0]==66);shot("onboarding-buttons");
 press(Button::A);expect("Lights");assert(leds[0][0]==66);press(Button::B);expect("2/11");press(Button::A);expect("Lights");assert(leds[0][0]==66);
 for(int i=0;i<6;++i)active->on_tick();assert(leds[1][0]==66);press(Button::A);expect("Tap to unlock");assert(leds[BadgeLed::BottomLeft][0]==66);active->on_tick();assert(leds[BadgeLed::BottomLeft][0]==66);shot("onboarding-tap");
 press(Button::A);expect("Shake it!");press(Button::A);expect("Shake it!");press(Button::B);expect("Tap to unlock");press(Button::A);shake=true;active->on_tick();press(Button::A);expect("Badge Connect");
 press(Button::B);expect("Shake it!");for(auto &led:leds)assert(led[0]==66);press(Button::A);expect("Badge Connect");
 press(Button::A);expect("8/11");expect("Bump to connect");expect("Bump together gently");
 expect("Only works while Connect app is open");
 auto *bump_copy=label(root,"Bump together gently");
 auto *bump_note=label(root,"Only works while Connect app is open");
 auto *bump_footer=label(root,"A / START: next   B: back");
 assert(lv_obj_get_height(bump_copy)==lv_font_montserrat_18.line_height);
 assert(lv_obj_get_height(bump_note)==lv_font_montserrat_14.line_height);
 lv_area_t copy_area,note_area,footer_area;
 lv_obj_get_coords(bump_copy,&copy_area);lv_obj_get_coords(bump_note,&note_area);lv_obj_get_coords(bump_footer,&footer_area);
 assert(copy_area.y1>129&&copy_area.y2<note_area.y1&&note_area.y2<footer_area.y1);
 for(int frame=0;frame<4;++frame){for(int i=0;i<12;++i)active->on_tick();refresh();}
 shot("onboarding-bump");press(Button::B);expect("Badge Connect");press(Button::A);expect("8/11");
 for(int step=0;step<11&&!label(root,"Badge rules");++step)press(Button::A);expect("11/11");auto *url=label(root,"badge.hackthenorth.com");assert(url&&lv_obj_get_height(url)==lv_font_montserrat_14.line_height);shot("onboarding-rules");
 press(Button::Start);assert(launched==nullptr);press(Button::A);press(Button::B);expect("Go explore");press(Button::A);press(Button::Start);assert(launched==nullptr);press(Button::A);press(Button::Start);assert(launched==app_badge_id_instance()&&shell==app_launcher_instance());
 assert(!badge_onboarding_required());enter(launched);
 for(auto &led:leds)for(int channel:led)assert(channel==0);
 enter(app_badge_id_instance());for(auto &led:leds)for(int channel:led)assert(channel==0);
 provisioned=false;hal_lights_clear();hal_lights_show();enter(app_badge_id_instance());
 for(auto &led:leds)for(int channel:led)assert(channel==0);
 puts("QR stays dark after onboarding and on re-entry; unregistered badge stays dark: PASS");
 provisioned=false;enter(app_onboarding_instance());press(Button::A);expect("1/11");provisioned=true;
 puts("Onboarding any-button welcome, back, eight-button flow, side-switch omission, shake gate, LEDs, agreement and URL: PASS");

 enter(app_mood_instance());expect("Mood LEDs");expect("Press A to choose your mood");shot("mood-intro");press(Button::A);expect("UP / DOWN: mood   START: info");press(Button::Down);
 for(int i=0;i<49;++i)active->on_tick();assert(leds[0][0]==33);
 for(int i=0;i<51;++i)active->on_tick();for(auto &led:leds)assert(led[0]==0&&led[1]==0&&led[2]==0);
 press(Button::Start);press(Button::B);expect("Press A to choose your mood");press(Button::A);
 for(int i=0;i<149;++i)active->on_tick();assert(leds[0][0]==33);shot("mood-picker");enter(app_mood_instance());expect("Press A to choose your mood");
 puts("Mood intro, help reopen, selection preservation and reduced 50% LED pulse: PASS");

 enter(app_bubble_wrap_instance());expect("ALL-TIME POPPED: 0");press(Button::A);expect("ALL-TIME POPPED: 1");
 assert(hal_config_get_int("bubblewrap","total_popped",0)==0);
 for(int i=1;i<24;++i)press(Button::A);expect("ALL POPPED!");expect("ALL-TIME POPPED: 24");
 lv_tick_inc(499);active->on_tick();assert(hal_config_get_int("bubblewrap","total_popped",0)==0);
 lv_tick_inc(1);active->on_tick();assert(hal_config_get_int("bubblewrap","total_popped",0)==24);
 press(Button::A);expect("24 LEFT");expect("ALL-TIME POPPED: 24");press(Button::B);expect("ALL-TIME POPPED: 25");shot("bubble-total");enter(app_mood_instance());enter(app_bubble_wrap_instance());expect("ALL-TIME POPPED: 25");
 assert(hal_config_get_int("bubblewrap","total_popped",0)==25);
 puts("Bubble sheets, immediate pop UI, coalesced idle/exit persistence and app re-entry: PASS");

 hal_config_set_string("workshop-quest","visited","");
 launched=nullptr;app_scanner_prepare_payload("work:openai-api");enter(app_scanner_instance());
 assert(launched==app_workshops_instance());enter(launched);expect("1/5");expect("COLLECTED: OAI");expect("OpenAI API Workshop");
 launched=nullptr;app_scanner_prepare_payload("work:openai-api");enter(app_scanner_instance());
 assert(launched==app_workshops_instance());enter(launched);expect("ALREADY COLLECTED");
 launched=nullptr;app_scanner_prepare_payload("work:not-a-workshop");enter(app_scanner_instance());
 assert(launched==nullptr);expect("Invalid workshop tag");
 time_us+=1500000;hal_lights_tick();
 puts("Default Scanner records workshop tags and opens Workshop Quest: PASS");

 hal_config_set_string("workshop-quest","visited","dryft-api,openai-api,zip-api,microsoft-api,internship-maxxing");
 enter(app_workshops_instance());expect("5/5");expect("Internship Maxxing Workshop");
 auto *workshop_grid=lv_obj_get_child(root,0);
 const char *completed_codes[]={"INT","ZIP","OAI","MS","DRY"};
 for(size_t i=0;i<5;++i)assert(strcmp(lv_label_get_text(lv_obj_get_child(workshop_grid,i)),completed_codes[i])==0);
 assert(strcmp(lv_label_get_text(lv_obj_get_child(workshop_grid,5)),"3MIN")==0);
 expect("SCAN TAG   D-PAD BROWSE   B PAGE 1/3");shot("workshops-completed-first");
 press(Button::B);expect("SCAN TAG   D-PAD BROWSE   B PAGE 2/3");press(Button::B);expect("SCAN TAG   D-PAD BROWSE   B PAGE 3/3");
 press(Button::B);expect("SCAN TAG   D-PAD BROWSE   B PAGE 1/3");expect("Internship Maxxing Workshop");
 puts("Workshop Quest keeps all completed workshops together on page one: PASS");

 enter(app_sponsors_instance());expect("Floor 1");expect("Floor 2");expect("Extended\nSponsor Bay");expect("--/64");shot("sponsor-floor-first");
 const uint32_t floor_children=lv_obj_get_child_count(root);active->on_tick();
 assert(lv_obj_get_child_count(root)==floor_children);active->on_tick();
 assert(lv_obj_get_child_count(root)>floor_children);
 for(int i=0;i<3;++i)active->on_tick();expect("0/64  0%");shot("sponsor-map");
 puts("Sponsor map paints floors first and adds every booth in four fast batches: PASS");

 app_sponsors_prepare_visit(1,true);enter(app_sponsors_instance());expect("SPONSOR DISCOVERED");
 auto *discovery_panel=lv_obj_get_parent(label(root,"SPONSOR DISCOVERED"));
 assert(lv_obj_get_width(discovery_panel)==286&&lv_obj_get_height(discovery_panel)==164);
 assert(lv_obj_get_style_shadow_width(discovery_panel,LV_PART_MAIN)==0);
 assert(lv_obj_get_style_transform_scale_x(discovery_panel,LV_PART_MAIN)==256);
 assert(lv_obj_get_style_transform_scale_y(discovery_panel,LV_PART_MAIN)==256);
 for(int i=0;i<12;++i)active->on_tick();refresh();
 assert(lv_obj_get_width(discovery_panel)==286&&lv_obj_get_height(discovery_panel)==164);
 assert(lv_obj_get_style_shadow_width(discovery_panel,LV_PART_MAIN)==0);
 assert(lv_obj_get_style_transform_scale_x(discovery_panel,LV_PART_MAIN)==256);
 assert(lv_obj_get_style_transform_scale_y(discovery_panel,LV_PART_MAIN)==256);
 shot("sponsor-discovered-static");press(Button::A);expect("Floor 1");
 puts("Sponsor discovery card remains fixed-size and shadow-free: PASS");

 test_connect();
 test_games();
 test_pairing();

 Toy ball("8",AppAccess::Public,&icon_eightball,true),light("LED",AppAccess::Internal,&icon_lights,true),bricks("BRK",AppAccess::Public,&icon_breakout,true),maze("MAZ",AppAccess::Public,&icon_maze,true),ttt("TTT",AppAccess::Public,&icon_tictactoe),flappy("FLP",AppAccess::Public,&icon_flappy,true),num("67");
 registry={&sponsors,&socials,app_rules_instance(),&num,&ball,&flashlight,&light,&bricks,&maze,&ttt,&flappy};
 launcher_slugs_set_mode(LauncherAppMode::PublicAndInternal);enter(app_launcher_instance());shot("launcher-icons");
 press(Button::Down);press(Button::Down);press(Button::Right);shot("launcher-diagnostic-focus");
 active->on_button({Button::A,ButtonEventKind::Pressed});press(Button::Right);shot("launcher-diagnostic-move");active->on_button({Button::A,ButtonEventKind::Released});
 puts("Launcher icons and diagnostic outlines render: PASS");
 return 0;
}
