#pragma once

#include "workshop_store.h"

class App;

App *app_workshops_instance();

// Scanner records the visit first, then prepares this app to acknowledge it.
void app_workshops_prepare_visit(const WorkshopVisitResult &visit);
