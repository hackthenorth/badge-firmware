// Real multiplayer/share engines; only clock and radio delivery are simulated.
void test_pairing()
{
    const uint8_t peer[6] = {1,2,3,4,5,6};
    auto deliver = [&](ButtonPair::Intent frame, int rssi = -40) {
        uint8_t bytes[ButtonPair::SIZE]; ButtonPair::encode(bytes, frame);
        radio_cb(peer, rssi, bytes, sizeof(bytes));
    };
    auto sent = [&]() {
        ButtonPair::Intent frame;
        assert(ButtonPair::decode(last_transmission.data(), last_transmission.size(), frame));
        return frame;
    };
    for (int scenario = 0; scenario < 6; ++scenario) {
        time_us = 1000000;
        assert(mp_session_begin({MPG_GAME_CHESS,300,-60,true}));
        mp_session_note_button(); auto local = sent();
        ButtonPair::Intent frame{900,local.nonce,MPG_GAME_CHESS,ButtonPair::Chess,{1,2,3,4,5,6}};
        time_us += scenario == 1 ? 1001000 : 100000;
        if(scenario==1)frame.ack_nonce=0;
        if(scenario==2)frame.context=2;
        if(scenario==3)frame.kind=ButtonPair::ShareReceiver;
        if(scenario==5)frame.kind=0;
        deliver(frame, scenario==4 ? -80 : -40); mp_session_tick();
        time_us=2500001;mp_session_tick();
        assert(mp_session_state() == (scenario==0 ? MpState::Proposed : MpState::Seeking));
        if(scenario==0){uint8_t actual[6];assert(mp_session_peer_mac(actual)&&memcmp(actual,peer,6)==0);}
        mp_session_end();
    }
    const char *tx_path = BADGE_QA_DIR "/share-pairing-tx.bin";
    const char *rx_path = BADGE_QA_DIR "/share-pairing-rx.bin";
    uint8_t bundle[10] = {};
    FILE *bundle_file = fopen(tx_path, "wb");
    assert(bundle_file && fwrite(bundle, 1, sizeof(bundle), bundle_file) == sizeof(bundle));
    fclose(bundle_file);
    std::vector<uint8_t> offer;
    for(int scenario=0;scenario<3;++scenario) {
        assert(share_xfer_start_send(tx_path,sizeof(bundle),0,"test","Test","1"));
        assert(share_xfer_state()==ShareState::Offering);
        offer=last_transmission;
        const auto *offered=reinterpret_cast<const share_hdr_t *>(offer.data());
        const uint16_t request=share_rd_u16(reinterpret_cast<const uint8_t *>(&offered->req_id));
        uint8_t accept[SHARE_MAX_FRAME] = {};
        auto *h=reinterpret_cast<share_hdr_t *>(accept);
        share_set_magic(h);h->ver=SHARE_VERSION;h->type=SHARE_TYPE_ACCEPT;h->total=1;
        share_wr_u16(reinterpret_cast<uint8_t *>(&h->req_id),request+(scenario==1));
        size_t n=share_accept_build(accept+SHARE_HDR_SIZE,SHARE_MAX_BODY,"Peer");
        if(scenario==2)n=0; // malformed acceptance body
        radio_cb(peer,-40,accept,SHARE_HDR_SIZE+n);share_xfer_tick();
        assert(share_xfer_state()==(scenario==0 ? ShareState::Sweeping : ShareState::Offering));
        share_xfer_abort();
    }
    assert(share_xfer_start_listen(rx_path));
    radio_cb(peer,-80,offer.data(),offer.size());share_xfer_tick();
    assert(share_xfer_state()==ShareState::Listening);
    radio_cb(peer,-40,offer.data(),offer.size());share_xfer_tick();
    assert(share_xfer_state()==ShareState::OfferReady);
    assert(share_xfer_accept_offer("Local"));
    assert(share_xfer_state()==ShareState::Accepting);
    assert(reinterpret_cast<const share_hdr_t *>(last_transmission.data())->type==SHARE_TYPE_ACCEPT);
    share_xfer_abort();
    std::filesystem::remove(tx_path);
    puts("Chess: mutual button intent; Share: offer/accept without bump, wrong offer, malformed acceptance and weak signal: PASS");
}
