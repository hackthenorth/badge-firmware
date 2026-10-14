#pragma once

#include "sponsor_store.h"

class App;

App *app_sponsors_instance();

// Scanner records the visit first, then prepares this app to acknowledge it.
// Mapped tables are focused; overflow tables intentionally have no grid cell.
void app_sponsors_prepare_visit(SponsorTableId table_id, bool is_new);
