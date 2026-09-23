/*
 * Stoatworks Labs - About window data for pitch.
 *
 * PROVISIONAL HAND COPY, in the shape stoatworks-backend/scripts/sync-about.py
 * generates (adapted from readout's generated header on 2026-09-23). The
 * project is not yet registered in the website's projects.json, so the sync
 * cannot produce this file. Once it is registered the sync overwrites this
 * file; edit it there, not here.
 *
 * `guide` is the fleet's guide URL for this slug, set by hand when
 * docs/USER-GUIDE.md was written, so the regenerated file carries the same four
 * buttons and the parameter count does not change after v0.1.0. An empty link
 * would drop the button and shift every parameter after it.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "pitch";
    inline constexpr auto slug = "pitch";
    inline constexpr auto hook = "An LED wall seen through a camera, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "https://stoatworks-labs.com/software/pitch/guide/";
    inline constexpr auto page = "https://stoatworks-labs.com/software/pitch/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/pitch";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
