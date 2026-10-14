#include "app.h"
#include "app_registry.h"
#include "app_reboot.h"
#include "hal_nfc.h"
#include "hal_radio.h"
#include "hal_buttons.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>

static lv_obj_t *root;
static std::function<void(ButtonEvent)> dispatch;
extern "C" lv_obj_t *hal_display_root(){return root;}
void hal_buttons_set_dispatcher(std::function<void(ButtonEvent)> fn){dispatch=fn;}
void hal_nfc_disable(){}
void hal_radio_disable(){}
bool hal_nfc_is_enabled(){return false;}
bool hal_radio_is_enabled(){return false;}
static App *reboot_requested;
static unsigned reboot_requests;
bool app_reboot_into_app(App *app){reboot_requested=app;++reboot_requests;return false;}
bool app_reboot_to_launcher(const char*){return false;}
void hal_sleep_note_activity(){}
void hal_sleep_wake_lock(bool){}
void hal_sleep_tick(){}
void hal_lights_flash_success(){}
void hal_lights_tick(){}
uint32_t hal_identity_sync_revision(){return 0;}

struct Screen:App {
    App *next=nullptr;
    std::vector<ButtonEvent> events;
    int checkpoints=0;
    const char *name()const override{return "test";}
    const char *slug()const override{return "test";}
    const char *icon()const override{return "T";}
    void on_enter(lv_obj_t*)override{events.clear();}
    void checkpoint_progress()override{checkpoints++;}
    void on_button(ButtonEvent ev)override{
        events.push_back(ev);
        if(next&&ev.button==Button::Start&&ev.kind==ButtonEventKind::Pressed)
            app_registry_launch(next);
    }
};

struct GameScreen:Screen {
    bool confirm_home_exit()const override{return true;}
};

struct RadioScreen:Screen {
    bool needs_fresh_boot()const override{return true;}
};
struct NfcScreen:Screen {
    bool uses_nfc()const override{return true;}
};

int main(){
    lv_init();auto *display=lv_display_create(320,240);
    static uint8_t pixels[320*240*4];
    lv_display_set_buffers(display,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display,[](lv_display_t *d,const lv_area_t*,uint8_t*){lv_display_flush_ready(d);});
    root=lv_obj_create(lv_screen_active());lv_obj_set_size(root,320,240);
    Screen onboarding,badge,launcher;onboarding.next=&badge;
    app_registry_start(&launcher,&onboarding);
    dispatch({Button::Start,ButtonEventKind::Pressed});
    dispatch({Button::Start,ButtonEventKind::Released});
    assert(onboarding.events.size()==1&&badge.events.empty());
    assert(onboarding.checkpoints==1);
    // The next deliberate Start press still reaches My Badge normally.
    dispatch({Button::Start,ButtonEventKind::Pressed});
    dispatch({Button::Start,ButtonEventKind::Released});
    assert(badge.events.size()==2);
    // HOME and another held button are both contained by an app transition.
    dispatch({Button::A,ButtonEventKind::Pressed});
    dispatch({Button::Home,ButtonEventKind::Pressed});
    dispatch({Button::Home,ButtonEventKind::Released});
    dispatch({Button::A,ButtonEventKind::Released});
    assert(launcher.events.empty());
    assert(badge.checkpoints==1);
    dispatch({Button::A,ButtonEventKind::Pressed});
    dispatch({Button::A,ButtonEventKind::Released});
    assert(launcher.events.size()==2);

    GameScreen game;
    app_registry_launch(&game);
    dispatch({Button::Home,ButtonEventKind::Pressed});
    assert(game.checkpoints==1); // Home pause shown.
    dispatch({Button::A,ButtonEventKind::Pressed}); // Cancel pause.
    dispatch({Button::A,ButtonEventKind::Released});
    dispatch({Button::Home,ButtonEventKind::Released});
    dispatch({Button::Home,ButtonEventKind::Pressed});
    dispatch({Button::Home,ButtonEventKind::Released});
    dispatch({Button::Home,ButtonEventKind::Pressed}); // Confirm exit.
    assert(game.checkpoints==3); // Both pauses plus the actual exit.

    RadioScreen radio;NfcScreen nfc;
    assert(!badge.needs_fresh_boot()&&radio.needs_fresh_boot()&&nfc.needs_fresh_boot());
    for(App *fresh:std::vector<App*>{&radio,&nfc}) {
        app_registry_return_to_launcher();
        unsigned before=reboot_requests;
        app_registry_launch(fresh);
        assert(reboot_requests==before+1&&reboot_requested==fresh);
        // Simulate the failed-persist fallback: direct app handoffs do not reboot.
        app_registry_launch(&badge);app_registry_launch(fresh);
        assert(reboot_requests==before+1);
        app_registry_return_to_launcher();
        app_registry_start(&launcher,fresh,true); // boot target bypasses once
        assert(reboot_requests==before+1);
        app_registry_return_to_launcher();app_registry_launch(fresh);
        assert(reboot_requests==before+2); // bypass was consumed
    }
    puts("App registry: held buttons, progress checkpoints, radio/NFC fresh entry, direct handoffs and one-shot reboot bypass: PASS");
}
