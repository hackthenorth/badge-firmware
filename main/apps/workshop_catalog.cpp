#include "workshop_catalog.h"

#include <cstring>

namespace {

constexpr WorkshopInfo kWorkshops[] = {
    // Main workshops first; sponsor-led sessions follow in their existing order.
    {"internship-maxxing",    "INT",  "Internship Maxxing Workshop"},
    {"win-in-three-minutes",  "3MIN", "How to Win in Three Minutes Workshop"},
    {"recruiting-101",        "REC",  "Recruiting 101 Workshop"},
    {"hacker-badge",          "BDG",  "Hacker Badge Workshop"},
    {"lelamp",                "LAMP", "LeLamp Workshop"},
    {"local-llms-101",        "LLM",  "Local LLMs 101 Workshop"},
    {"prompt-to-pwn",         "PWN",  "Prompt to Pwn Workshop"},

    // Sponsor workshops, including MLH sessions.
    {"federato-api",          "FED",  "Federato API Workshop"},
    {"td-api",                "TD",   "TD API Workshop"},
    {"warp-api",              "WARP", "Warp API Workshop"},
    {"rbc-api",               "RBC",  "RBC API Workshop"},
    {"zip-api",               "ZIP",  "Zip API Workshop"},
    {"google-api",            "GGL",  "Google API Workshop"},
    {"expo-api",              "EXPO", "Expo API Workshop"},
    {"unto-labs-api",         "UNTO", "Unto Labs API Workshop"},
    {"rox-api",               "ROX",  "Rox API Workshop"},
    {"qnx-api",               "QNX",  "QNX API Workshop"},
    {"bracketbot-api",        "BKT",  "BracketBot API Workshop"},
    {"solana-api",            "SOL",  "Solana API Workshop"},
    {"elastic-api",           "ELS",  "Elastic API Workshop"},
    {"cse-api",               "CSE",  "CSE API Workshop"},
    {"openai-api",            "OAI",  "OpenAI API Workshop"},
    {"composio-api",          "CMP",  "Composio API Workshop"},
    {"sentry-api",            "SEN",  "Sentry API Workshop"},
    {"microsoft-api",         "MS",   "Microsoft API Workshop"},
    {"mlh-github-copilot",    "COP",  "MLH: Hacking with GitHub Copilot"},
    {"mlh-google-ai-studio",  "GAI",  "MLH: Intro to Google AI Studio"},
    {"dominion-dynamics-api", "DOM",  "Dominion Dynamics API Workshop"},
    {"virtu-api",             "VIRT", "Virtu API Workshop"},
    {"tether-api",            "TET",  "Tether API Workshop"},
    {"gptzero-api",           "GPT0", "GPTZero API Workshop"},
    {"browserbase-api",       "BRWS", "Browserbase API Workshop"},
    {"linq-api",              "LINQ", "Linq API Workshop"},
    {"logitech-api",          "LOGI", "Logitech API Workshop"},
    {"amazon-api",            "AMZ",  "Amazon API Workshop"},
    {"huawei-api",            "HUA",  "Huawei API Workshop"},
    {"dryft-api",             "DRY",  "Dryft API Workshop"},
};

} // namespace

size_t workshop_catalog_count()
{
    return sizeof(kWorkshops) / sizeof(kWorkshops[0]);
}

const WorkshopInfo *workshop_catalog_at(size_t index)
{
    return index < workshop_catalog_count() ? &kWorkshops[index] : nullptr;
}

const WorkshopInfo *workshop_catalog_find_slug(const char *slug)
{
    if (!slug) return nullptr;
    for (const WorkshopInfo &workshop : kWorkshops) {
        if (strcmp(workshop.slug, slug) == 0) return &workshop;
    }
    return nullptr;
}

size_t workshop_catalog_index(const WorkshopInfo *workshop)
{
    if (!workshop) return workshop_catalog_count();
    for (size_t i = 0; i < workshop_catalog_count(); ++i) {
        if (&kWorkshops[i] == workshop) return i;
    }
    return workshop_catalog_count();
}
