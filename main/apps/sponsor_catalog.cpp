#include "sponsor_catalog.h"

#include <cstring>

namespace {

// Sponsor identity is deliberately independent of physical table assignment.
// A valid booth scan supplies the current table and persists that association.
// Names and links follow the current in-person roster. Descriptions are kept
// short enough for the badge detail view; sponsorship can revise this catalog
// independently of the event-day booth layout.
constexpr SponsorInfo kSponsors[] = {
    {"federato", "Federato", "AI-native platform for insurance underwriting and policy lifecycle workflows.", "https://www.federato.ai/"},
    {"rbc", "RBC", "Financial institution spanning banking, wealth, insurance, and capital markets.", "https://jobs.rbc.com/ca/en/early-tech-talent"},
    {"dominion", "Dominion Dynamics", "Builds autonomous systems, sensing, and communications for extreme operations.", "https://defendthedominion.com"},
    {"shopify", "Shopify", "Commerce platform for starting, running, and growing businesses.", "https://internships.shopify.com/"},
    {"amazon", "Amazon", "Annapurna Labs designs Amazon's custom chips for AWS compute and AI.", "https://amazon.jobs/content/en/teams/annapurna-labs"},
    {"logitech", "Logitech", "Designs hardware and software that help people create, work, play, and connect.", "https://www.logitech.com/en-ca"},
    {"td", "TD", "North American bank providing personal, business, wealth, and markets services.", "https://www.td.com/ca/en/personal-banking"},
    {"sentry", "Sentry", "Application monitoring for errors, performance, tracing, replay, and logs.", "https://sentry.io/welcome/"},
    {"hrt", "HRT", "Quantitative trading firm using advanced computing across global markets.", "https://www.hudsonrivertrading.com/"},
    {"solana", "Solana", "High-performance blockchain and ecosystem for decentralized applications.", "https://solana.com/"},
    {"netic", "Netic", "Builds agentic AI that automates revenue-generating enterprise workflows.", "https://www.netic.ai/"},
    {"browserbase", "Browserbase", "Cloud browser infrastructure for web automation and AI agents.", "https://www.browserbase.com/"},
    {"intact", "Intact", "Property, casualty, and specialty insurance provider across North America.", "https://www.intactfc.com/careers"},
    {"citadel", "Citadel", "Investment manager powered by research, technology, and quantitative expertise.", "https://www.citadel.com/"},
    {"tether", "Tether", "Builds digital infrastructure across finance, AI, energy, and communications.", "https://tether.io/"},
    {"rox", "Rox", "AI-native revenue platform for sales research, planning, and execution.", "https://www.rox.com/"},
    {"gptzero", "GPTZero", "AI-content detection and authorship verification for schools and organizations.", "https://gptzero.me"},
    {"google", "Google", "Builds search, cloud, AI, productivity, and consumer technology products.", "https://google.com/careers"},
    {"sun-life", "Sun Life", "Financial services company offering insurance, wealth, health, and asset management.", "https://www.sunlife.ca/en/careers/"},
    {"qnx", "QNX", "Real-time operating systems and embedded software for safety-critical products.", "https://qnx.software/en/developers/get-started/qnx-everywhere"},
    {"rundoo", "Rundoo", "AI-first operating system for independent building-material retailers.", "https://rundoo.ai/"},
    {"omen", "Omen", "Uses real-time fluid analysis to predict failures in critical equipment.", "https://omen.ai"},
    {"virtu", "Virtu Financial", "Electronic market maker providing liquidity, execution, and analytics worldwide.", "https://www.virtu.com/"},
    {"phoebe", "Phoebe", "AI scheduling teammate that helps home-care agencies fill and manage shifts.", "https://www.phoebe.work/"},
    {"composio", "Composio", "Integration infrastructure that lets AI agents act across apps and APIs.", "https://composio.dev"},
    {"rilla", "Rilla", "Conversation intelligence platform for field-service teams.", "https://www.rilla.com/"},
    {"cognition", "Cognition", "AI lab behind Devin, an autonomous software-engineering agent.", "https://cognition.com/"},
    {"clipboard", "Clipboard", "Marketplace connecting healthcare professionals with open facility shifts.", "https://www.clipboardworks.com/"},
    {"microsoft", "Microsoft", "Builds productivity software, cloud platforms, devices, and AI.", "https://www.microsoft.com/"},
    {"offdeal", "OffDeal", "Technology-enabled investment bank helping small-business owners sell.", "https://offdeal.io"},
    {"rocket", "Rocket Companies", "Canadian technology hub building digital products for Rocket Companies.", "https://rocketinnovationstudio.ca/"},
    {"runpod", "Runpod", "Developer cloud for training, deploying, and scaling AI on demand GPUs.", "https://www.runpod.io/"},
    {"bloomberg", "Bloomberg", "Provides financial data, news, analytics, and technology to global markets.", "https://www.bloomberg.com/company/"},
    {"jaza-energy", "Jaza Energy", "Builds distributed solar-energy networks for underserved communities.", "https://www.jazaenergy.com"},
    {"cubist", "Cubist Systematic Strategies", "Point72 business using quantitative research and technology to invest.", "https://point72.com/"},
    {"afterquery", "AfterQuery", "Creates expert data, evaluations, and reinforcement-learning environments for AI.", "https://www.afterquery.com/"},
    {"unto-labs", "Unto Labs", "Builds Thru, a high-performance base-layer blockchain.", "https://thru.org/"},
    {"zip", "Zip", "AI-powered procurement orchestration platform for business purchasing.", "https://zip.com"},
    {"cse", "CSE", "Canada's national cryptologic agency for cybersecurity and signals intelligence.", "https://www.cse-cst.gc.ca/en"},
    {"jane-street", "Jane Street Capital", "Quantitative trading firm using technology and research across global markets.", "https://www.janestreet.com/"},
    {"cohere", "Cohere", "Enterprise AI platform for language models, search, and agents.", "https://cohere.com/"},
    {"accenture", "Accenture", "Professional services company specializing in strategy, technology, and operations.", "http://accenture.com/"},
    {"legion-health", "Legion Health", "Digital-first psychiatry platform offering telehealth and AI-enabled workflows.", "https://legionhealth.com"},
    {"aramco-americas", "Aramco Americas", "Supports energy research, technology, and business operations in the Americas.", "https://americas.aramco.com/"},
    {"huawei", "Huawei", "Develops telecommunications networks, devices, cloud, and AI technology.", "https://www.huawei.com/en/"},
    {"genesys", "Genesys", "Cloud platform for AI-powered customer experience and contact centers.", "https://www.genesys.com/"},
    {"uber", "Uber", "Technology platform for rides, delivery, freight, and on-demand services.", "https://jobs.uber.com/"},
    {"linq", "Linq", "Developer APIs for iMessage, RCS, SMS, voice, and conversational AI.", "https://linqapp.com/"},
    {"scale-ai", "Scale AI", "Data and infrastructure platform for developing, evaluating, and deploying AI.", "https://scale.com/careers"},
    {"uw-math", "UW Math", "University of Waterloo faculty for math, statistics, and computer science.", "https://uwaterloo.ca/math"},
    {"persona", "Persona", "Identity platform for verification, fraud prevention, and compliance workflows.", "https://withpersona.com/"},
    {"elastic", "Elastic", "Search AI platform for search, observability, and security workloads.", "https://www.elastic.co/"},
    {"cwp-energy", "CWP Energy", "Renewable-energy trading and analytics in North American power markets.", "https://mcgillstlaurent.com/cwp-energy/"},
    {"cloudflare", "Cloudflare", "Connectivity cloud for securing, accelerating, and operating networks.", "https://www.cloudflare.com/startups/"},
    {"attention", "Attention", "AI sales platform for call follow-up, CRM updates, coaching, and insights.", "https://www.attention.com/"},
    {"baseten", "Baseten", "AI infrastructure platform for high-performance model inference and deployment.", "https://www.baseten.co/"},
    {"openai", "OpenAI", "AI research and deployment company building models and developer platforms.", "https://jobs.ashbyhq.com/openai/form/openai-emt-talent-community"},
    {"warp", "Warp", "Agentic development environment combining an intelligent terminal and coding tools.", "https://www.warp.dev/"},
    {"dryft", "Dryft", "AI operating system for decisions in manufacturing operations.", "https://dryft.ai/"},
    {"expo", "Expo", "React Native framework and cloud services for building universal apps.", "https://expo.dev/home"},
    {"trajectory", "Trajectory", "Continual-learning platform that improves AI products from real usage and feedback.", "https://www.trajectory.ai/"},
    {"decagon", "Decagon", "AI agents for customer support across voice, chat, email, and SMS.", "https://decagon.ai/"},
    {"sequence-holdings", "Sequence Holdings", "Holding company applying AI and engineering to service businesses.", "https://seqholdings.com/"},
    {"mlh", "MLH", "Major League Hacking supports hackers through hackathons, fellowships, and developer programs.", "https://www.mlh.com/"},
};

} // namespace

size_t sponsor_catalog_count()
{
    return sizeof(kSponsors) / sizeof(kSponsors[0]);
}

const SponsorInfo *sponsor_catalog_at(size_t index)
{
    return index < sponsor_catalog_count() ? &kSponsors[index] : nullptr;
}

const SponsorInfo *sponsor_catalog_find_slug(const char *slug)
{
    if (!slug) return nullptr;
    for (const auto &sponsor : kSponsors) {
        if (strcmp(sponsor.slug, slug) == 0) return &sponsor;
    }
    return nullptr;
}
