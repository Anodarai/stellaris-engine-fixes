// Part of numericfixes_runtime.inl: diplomatic_weight_arithmetic. The base weights
// (diplomacyoverflow.h), then what follows them: the diplomatic display (diplodisplay.h), federation
// sums (federationsums.h), votes (diplovotes.h), scores (diploscores.h), and a country's totals
// (countrytotals.h), which fleet_power_arithmetic shares.

// ---- diplomatic_weight_arithmetic: the base weights ----

diplomacyoverflow::Locations g_diplomacyOverflow;
bool g_diplomacyArithmeticInstalled = false;

// The parts of diplomatic_weight_arithmetic installed after the base weights (display, votes,
// federation sums, scores, population weight) go in only with them, never as a partial feature.
bool DiplomacyPartWanted(HMODULE self, const char* component) {
    if (!NumericFix(self, "diplomatic_weight_arithmetic"))
        return false;
    if (g_diplomacyArithmeticInstalled)
        return true;
    Log(LogLevel::Error, component, "off: the base diplomatic weight arithmetic did not install");
    return false;
}

void InstallDiplomacyArithmetic(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "diplomatic_weight_arithmetic")) {
        Log(LogLevel::Info, "diplomatic_arithmetic", "off (diplomatic_weight_arithmetic=0)");
        return;
    }
    std::string error;
    if (!diplomacyoverflow::Locate(img, g_diplomacyOverflow, error)) {
        Log(LogLevel::Error, "diplomatic_arithmetic", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_diplomacyOverflow.blocks);
    if (!blocks.Prepare()) {
        Log(LogLevel::Error, "diplomatic_arithmetic", "cannot prepare stubs; off");
        return;
    }
    // Conversion is part of this feature: otherwise a wrapped input reaches the new products.
    if (!InstallFixedPointConversion(self, img, true)) {
        Log(LogLevel::Error, "diplomatic_arithmetic", "conversion prerequisite failed; off");
        return;
    }
    if (!blocks.Write()) {
        Log(LogLevel::Error, "diplomatic_arithmetic",
            "installation failed; off (fixed-point conversion remains enabled)");
        return;
    }
    g_diplomacyArithmeticInstalled = true;
    Log(LogLevel::Info, "diplomatic_arithmetic",
        "19 wide products; exact component/modifier totals; economic conversion and borrowing boundaries");
}

// ---- diplomatic_weight_arithmetic: the parts that follow the base weights ----

diplodisplay::Locations g_diploDisplay;

void InstallDiplomaticDisplay(HMODULE self, const locate::Image& image) {
    if (!DiplomacyPartWanted(self, "diplomatic_display"))
        return;
    std::string error;
    if (!g_diplomacyOverflow.functions[0] || !g_diplomacyOverflow.functions[1] ||
        !diplodisplay::Locate(image, g_diploDisplay, error, g_diplomacyOverflow.functions)) {
        Log(LogLevel::Error, "diplomatic_display", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_diploDisplay.blocks);
    if (!InstallBlocks(blocks, "diplomatic_display"))
        return;
    Log(LogLevel::Info, "diplomatic_display",
        "wide localized base, component, modifier, borrowed and federation values");
}

diplovotes::Locations g_diploVotes;
federationsums::Locations g_federationSums;

void InstallFederationSums(HMODULE self, const locate::Image& image) {
    if (!DiplomacyPartWanted(self, "federation_sums"))
        return;
    std::string error;
    if (!federationsums::Locate(image, g_federationSums, error)) {
        Log(LogLevel::Error, "federation_sums", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_federationSums.blocks);
    if (!InstallBlocks(blocks, "federation_sums"))
        return;
    Log(LogLevel::Info, "federation_sums", "exact AI acceptance, predictions and law/type views");
}

void InstallDiplomaticVotes(HMODULE self, const locate::Image& img) {
    if (!DiplomacyPartWanted(self, "diplomatic_votes"))
        return;
    std::string error;
    if (!diplovotes::Locate(img, g_diploVotes, error)) {
        Log(LogLevel::Error, "diplomatic_votes", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_diploVotes.blocks);
    if (!InstallBlocks(blocks, "diplomatic_votes"))
        return;
    Log(LogLevel::Info, "diplomatic_votes", "wide resolution/federation votes and coupled community totals");
}

void InstallDiplomaticScores(HMODULE self, const locate::Image& img) {
    if (!DiplomacyPartWanted(self, "diplomatic_scores"))
        return;
    std::string error;
    if (!diploscores::Locate(img, g_diploScores, error)) {
        Log(LogLevel::Error, "diplomatic_scores", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_diploScores.blocks);
    if (!InstallBlocks(blocks, "diplomatic_scores"))
        return;
    Log(LogLevel::Info, "diplomatic_scores", "exact technology, economy, job weights and individual/total favor loans");
}

// ---- fleet_power_arithmetic and diplomatic_weight_arithmetic: a country's totals ----

countrytotals::Locations g_countryTotals;

void InstallCountryTotals(HMODULE self, const locate::Image& img) {
    bool fleet = NumericFix(self, "fleet_power_arithmetic");
    bool population = DiplomacyPartWanted(self, "country_totals");
    if (!fleet && !population)
        return;
    std::string error;
    if (!countrytotals::Locate(img, g_countryTotals, error)) {
        Log(LogLevel::Error, "country_totals", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    bool enabled[] = {fleet, fleet, population};
    for (int i = 0; i < 3; ++i) {
        if (enabled[i])
            blocks.Add(g_countryTotals.blocks[i]);
    }
    if (!InstallBlocks(blocks, "country_totals"))
        return;
    Log(LogLevel::Info, "country_totals", "wide fleet totals=%d; wide population weight=%d", fleet, population);
}
