// Two copies of the real firmware engine, with lossy radio registers and real disk IO.
#include "engine.h"
#include "app_share_proto.h"
#include "share_bundle.h"
#include "hal_radio.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

static int64_t clock_us;
static int endpoint;
static bool short_io;
static int write_error;
ssize_t test_write(int fd, const void *p, size_t n) {
    assert(n <= SHARE_STREAM_CHUNK_LEN);
    if (write_error && endpoint == 1) { errno = write_error; return -1; }
    return write(fd, p, short_io && n > 37 ? 37 : n);
}
ssize_t test_read(int fd, void *p, size_t n) {
    assert(n <= SHARE_STREAM_CHUNK_LEN);
    return read(fd, p, short_io && n > 37 ? 37 : n);
}
struct Radio {
    std::vector<uint8_t> frame;
    bool advertising = false;
    uint32_t adv_min = 0, adv_max = 0, scan_itvl = 0, scan_window = 0;
} radios[2];
static const uint8_t macs[3][6] = {{1,1,1,1,1,1}, {2,2,2,2,2,2}, {3,3,3,3,3,3}};
int64_t esp_timer_get_time() { return clock_us; }
esp_err_t hal_radio_enable() { return ESP_OK; }
void hal_radio_set_recv_cb(RadioRecvCb) {}
void hal_radio_reset_timings() {}
void hal_radio_local_mac(uint8_t *out) { memcpy(out, macs[endpoint], 6); }
esp_err_t hal_radio_set_adv_interval_ms(uint32_t mn, uint32_t mx) {
    radios[endpoint].adv_min = mn;
    radios[endpoint].adv_max = mx;
    return ESP_OK;
}
esp_err_t hal_radio_set_scan_duty_ms(uint32_t itvl, uint32_t window) {
    radios[endpoint].scan_itvl = itvl;
    radios[endpoint].scan_window = window;
    return ESP_OK;
}
esp_err_t hal_radio_adv_pause() { radios[endpoint].advertising = false; return ESP_OK; }
esp_err_t hal_radio_adv_resume() { radios[endpoint].advertising = true; return ESP_OK; }
esp_err_t hal_radio_send(const void *p, size_t n) {
    assert(n <= SHARE_MAX_FRAME);
    auto bytes = static_cast<const uint8_t *>(p);
    radios[endpoint].frame.assign(bytes, bytes + n);
    return ESP_OK;
}
static void write_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary); f.write((const char *)data.data(), data.size()); assert(f.good());
}
static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static uint8_t type(const std::vector<uint8_t> &f) { return f.empty() ? 0 : f[5]; }
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string tx = std::string(argv[1]) + "/tx.bin", rx_base = std::string(argv[1]) + "/rx.bin";
    Engine a = sender_engine(), b = receiver_engine();
    // Small, 1 KiB boundaries, full 48 KiB, loss/duplicates/reordering,
    // corrupted data, truncated source, cancellation, lost final ACK, storage failure.
    for (int scenario = 0; scenario < 13; ++scenario) {
        // Scenario 11 is a fresh badge: its apps directory does not exist.
        const std::string rx = scenario == 11
            ? std::string(argv[1]) + "/apps/.share-rx.bin" : rx_base;
        size_t sizes[] = {6, 1023, 1024, 1025, SHARE_BUNDLE_MAX_LEN};
        size_t size = scenario < 5 ? sizes[scenario] : SHARE_BUNDLE_MAX_LEN;
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(i * 31 + i / 257);
        write_file(tx, bytes);
        radios[0] = {}; radios[1] = {};
        short_io = scenario == 5; write_error = 0;
        endpoint = 0; assert(a.send(tx.c_str(), size, share_bundle_crc32(bytes.data(), size), "demo", "Demo", "1"));
        assert(a.state() == ShareState::Offering && radios[0].advertising);
        assert(type(radios[0].frame) == SHARE_TYPE_OFFER);
        assert(radios[0].adv_min == 30 && radios[0].scan_itvl == 100 && radios[0].scan_window == 100);
        endpoint = 1;
        assert(b.listen(rx.c_str()));
        assert(b.state() == ShareState::Listening && !radios[1].advertising);
        // Idle listen stays at the 20% default scan duty; the offer is
        // persistent so this still latches, at a fraction of the draw.
        assert(radios[1].scan_itvl == 150 && radios[1].scan_window == 30);
        auto offer = radios[0].frame;
        // Malformed OFFER and truncated header must never latch.
        auto bad = offer; bad[SHARE_HDR_SIZE + 6]++;
        b.receive(macs[0], -30, bad.data(), bad.size()); b.tick();
        assert(b.state() == ShareState::Listening);
        b.receive(macs[0], -30, offer.data(), SHARE_HDR_SIZE + 9); b.tick();
        assert(b.state() == ShareState::Listening);
        b.receive(macs[0], -30, offer.data(), offer.size()); b.tick();
        assert(b.state() == ShareState::OfferReady);
        assert(b.accept("Receiver"));
        assert(b.state() == ShareState::Accepting && radios[1].advertising);
        assert(type(radios[1].frame) == SHARE_TYPE_ACCEPT);
        // Acceptance escalates to the transfer tune the DATA dwell assumes.
        assert(radios[1].adv_min == 30 && radios[1].scan_itvl == 100 && radios[1].scan_window == 100);
        auto accept = radios[1].frame;
        endpoint=0; a.receive(macs[1], -30, accept.data(), accept.size());
        if (scenario == 7) assert(truncate(tx.c_str(), 0) == 0);
        bool cancelled = false;
        int final_drops = 0;
        std::vector<uint8_t> delayed_data, delayed_stat;
        for (int tick = 0; tick < 4000; ++tick) {
            clock_us += 20000;
            if (tick == 30) write_error = scenario == 10 ? ENOSPC : scenario == 12 ? ENOMEM : 0;
            endpoint=0; a.tick();
            auto data = radios[0].frame;
            if (radios[0].advertising && type(data) == SHARE_TYPE_DATA) {
                auto *h = (share_hdr_t *)data.data();
                endpoint=1;
                if (scenario == 6 && h->seq == 0) data.back() ^= 1;
                // A different badge with the same request ID cannot contaminate the file.
                auto foreign = data; foreign.back() ^= 1;
                b.receive(macs[2], -30, foreign.data(), foreign.size());
                if (scenario != 5 || tick % 5 != 0) {
                    b.receive(macs[0], -30, data.data(), data.size());
                    if (scenario == 5 && tick % 7 == 0) b.receive(macs[0], -30, data.data(), data.size());
                }
                if (scenario == 5 && !delayed_data.empty() && tick % 11 == 0)
                    b.receive(macs[0], -30, delayed_data.data(), delayed_data.size());
                if (tick % 13 == 0) delayed_data=data;
            } else if (radios[0].advertising && type(data) == SHARE_TYPE_DONE) {
                endpoint=1; b.receive(macs[0], -30, data.data(), data.size());
            }
            endpoint=1; b.tick();
            if (scenario == 8 && tick == 30) { b.abort(); cancelled=true; break; }
            auto stat=radios[1].frame;
            if (radios[1].advertising && type(stat) == SHARE_TYPE_STAT) {
                auto *st=(const share_stat_t *)(stat.data()+SHARE_HDR_SIZE);
                if (b.state()==ShareState::Verifying) assert(st->recv_count < st->total);
                bool drop = (scenario == 5 && tick % 4 == 0);
                if (scenario == 9 && st->recv_count == st->total && final_drops++ < 20) drop=true;
                endpoint=0;
                // Lying count must not cause false success.
                auto bad_stat=stat; bad_stat[SHARE_HDR_SIZE+1]=255;
                a.receive(macs[1], -30, bad_stat.data(), bad_stat.size());
                if (!drop) a.receive(macs[1], -30, stat.data(), stat.size());
                if (scenario == 5 && !delayed_stat.empty() && tick % 11 == 0)
                    a.receive(macs[1], -30, delayed_stat.data(), delayed_stat.size());
                if (tick % 13 == 0) delayed_stat=stat;
            }
            if (a.state()==ShareState::Failed || b.state()==ShareState::Failed) break;
            if (a.state()==ShareState::Done && b.state()==ShareState::Done) break;
        }
        if (scenario == 6) {
            assert(b.state()==ShareState::Failed && b.fail()==ShareFail::Crc);
            assert(!b.file(nullptr) && access(rx.c_str(), F_OK)!=0);
            assert(a.state()!=ShareState::Done);
        } else if (scenario == 7) {
            assert(a.state()==ShareState::Failed && a.fail()==ShareFail::Storage);
        } else if (scenario == 10 || scenario == 12) {
            assert(b.state()==ShareState::Failed);
            assert(b.fail() == (scenario == 10 ? ShareFail::StorageFull : ShareFail::NoSpace));
            assert(access(rx.c_str(), F_OK)!=0);
        } else if (scenario == 8) {
            assert(cancelled && access(rx.c_str(), F_OK)!=0);
        } else {
            assert(a.state()==ShareState::Done && b.state()==ShareState::Done);
            size_t n=0; assert(b.file(&n) && n==size);
            assert(read_file(rx)==bytes);
        }
        endpoint=1; b.abort(); assert(access(rx.c_str(), F_OK)!=0);
        endpoint=0; a.abort();
    }
    const std::string rx = rx_base;
    write_error = 0;
    // A non-directory parent is a file-access error, not full storage.
    endpoint=1; assert(b.listen((tx + "/rx.bin").c_str()));
    assert(b.state()==ShareState::Failed && b.fail()==ShareFail::Storage);
    b.abort();

    // An offer remains advertised beyond 30 seconds, with no synchronized
    // press. A matching offer refreshes the prompt, but DATA without local
    // acceptance cannot create a file or elicit ACCEPT/STAT.
    write_file(tx, std::vector<uint8_t>(1024, 1));
    endpoint=0; assert(a.send(tx.c_str(),1024,0,"demo","Demo","1"));
    auto offer=radios[0].frame;
    endpoint=1; assert(b.listen(rx.c_str()));
    b.receive(macs[0],-80,offer.data(),offer.size()); b.tick();
    assert(b.state()==ShareState::Listening);
    b.receive(macs[0],127,offer.data(),offer.size()); b.tick();
    assert(b.state()==ShareState::Listening);
    for(int i=0;i<3100;++i) {
        endpoint=0; a.tick();
        assert(a.state()==ShareState::Offering && radios[0].advertising);
        endpoint=1;
        b.receive(macs[0],-30,offer.data(),offer.size()); b.tick();
        assert(b.state()==ShareState::OfferReady && !radios[1].advertising);
    }
    auto unsolicited=offer;
    unsolicited[5]=SHARE_TYPE_DATA;
    unsolicited.resize(SHARE_HDR_SIZE + SHARE_CHUNK_LEN);
    b.receive(macs[0],-30,unsolicited.data(),unsolicited.size()); b.tick();
    assert(b.state()==ShareState::OfferReady && access(rx.c_str(),F_OK)!=0);
    assert(!radios[1].advertising);
    // Another badge cannot extend the original offer's lifetime.
    for(int i=0;i<1501;++i) {
        b.receive(macs[2],-30,offer.data(),offer.size()); b.tick();
        if (b.state()==ShareState::Listening) break;
    }
    assert(b.state()==ShareState::Listening && !b.accept("Too late"));
    b.receive(macs[0],-30,offer.data(),offer.size()); b.tick();
    assert(b.accept("Receiver"));
    assert(b.state()==ShareState::Accepting && type(radios[1].frame)==SHARE_TYPE_ACCEPT);
    // Sender never delivers DATA: fail and release the staging file.
    for(int i=0;i<501;++i) b.tick();
    assert(b.state()==ShareState::Failed && b.fail()==ShareFail::Lost);
    assert(access(rx.c_str(),F_OK)!=0);
    b.abort(); endpoint=0; a.abort(); unlink(tx.c_str());
    puts("Share streaming: first-app receive, one-press acceptance, persistent offers, consent, 1 KiB boundaries, 48 KiB, loss/reorder/duplicates, peer binding, CRC, final ACK, storage/memory errors, cancellation and timeout: PASS");
}
