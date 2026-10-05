/**
 * @file Ps1GameUIs.cpp
 * @brief Tools > Recomp > <Game> > Create ... UI (see Ps1GameUIs.h).
 */

#include "Ps1GameUIs.h"

#if EDITOR

#include "Ps1Widgets.h"
#include "Wasm/ps1w_module.h"

#include "Engine.h"
#include "Log.h"
#include "World.h"
#include "Nodes/Widgets/Canvas.h"
#include "Nodes/Widgets/Quad.h"
#include "Nodes/Widgets/Text.h"
#include "Nodes/Widgets/Widget.h"
#include "Plugins/EditorUIHooks.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
const glm::vec4 kPanelColor = {0.04f, 0.05f, 0.08f, 0.85f};
const glm::vec4 kTextColor = {1.0f, 1.0f, 1.0f, 1.0f};
const glm::vec4 kDimColor = {0.65f, 0.75f, 0.9f, 1.0f};
const glm::vec4 kHeaderColor = {1.0f, 0.85f, 0.35f, 1.0f};
const float kFontSize = 16.0f;
const float kRow = 24.0f;

// Builds a UI by name: a child that already exists is reused as it is (whatever the
// user changed on it); only missing children are created and given their defaults.
struct Builder
{
    int added = 0;
    int kept = 0;

    // The child `name` of `parent`, created with `init` if missing. Returns nullptr when
    // a node of that name exists but has another type (the user replaced it): it is
    // left alone, and so is everything that would go inside it.
    template <class T>
    T* Ensure(Node* parent, const char* name, const std::function<void(T*)>& init)
    {
        if (parent == nullptr)
        {
            return nullptr;
        }
        if (Node* existing = parent->FindChild(name, false))
        {
            ++kept;
            return existing->As<T>();
        }
        T* node = parent->CreateChild<T>(name);
        init(node);
        ++added;
        return node;
    }
};

void Place(Widget* w, float x, float y, float width, float height)
{
    w->SetAnchorMode(AnchorMode::TopLeft);
    w->SetPosition(x, y);
    w->SetDimensions(width, height);
}

void Style(Text* t, float size, glm::vec4 color)
{
    t->SetTextSize(size);
    t->SetColor(color);
    t->SetHorizontalJustification(Justification::Left);
    t->SetVerticalJustification(Justification::Center);
}

// A see-through grouping container (Widget): alpha stays 1 so children still draw.
Widget* Group(Builder& b, Node* parent, const char* name, float x, float y, float w, float h)
{
    return b.Ensure<Widget>(parent, name, [&](Widget* g) { Place(g, x, y, w, h); });
}

void Label(Builder& b, Node* parent, const char* name, const char* text, float x, float y, float w,
           float size = kFontSize, glm::vec4 color = kTextColor)
{
    b.Ensure<Text>(parent, name, [&](Text* t) {
        Place(t, x, y, w, kRow);
        Style(t, size, color);
        t->SetText(text);
    });
}

void Bound(Builder& b, Node* parent, const char* name, const char* format, float x, float y, float w,
           float size = kFontSize, glm::vec4 color = kTextColor, bool hideIfMissing = false)
{
    b.Ensure<Ps1Text>(parent, name, [&](Ps1Text* t) {
        Place(t, x, y, w, kRow);
        Style(t, size, color);
        t->SetFormat(format);
        t->SetHideIfMissing(hideIfMissing);
    });
}

void Bar(Builder& b, Node* parent, const char* name, const char* variable, const char* maxVariable, float x, float y,
         float w, glm::vec4 fill)
{
    b.Ensure<Ps1Bar>(parent, name, [&](Ps1Bar* bar) {
        Place(bar, x, y + 3.0f, w, kRow - 6.0f);
        bar->SetShowPercentage(false);
        bar->SetFillColor(fill);
        bar->SetBackgroundColor({0.15f, 0.15f, 0.18f, 1.0f});
        bar->SetVariables(variable, maxVariable);
    });
}

// A cheat switch: a Button (gamepad navigation reaches Buttons, not CheckBoxes) that
// toggles the variable and shows its state, "Infinite HP: ON".
Ps1Button* ToggleButton(Builder& b, Node* parent, const char* name, const char* label, const char* variable, float y)
{
    return b.Ensure<Ps1Button>(parent, name, [&](Ps1Button* btn) {
        Place(btn, 0.0f, y, 272.0f, kRow + 2.0f);
        btn->SetTextString(std::string(label) + ": --");
        btn->SetToggle(variable, 1);
        btn->SetLabelFormat(std::string(label) + ": {" + variable + "?ON|OFF}");
    });
}

Ps1Button* RequestButton(Builder& b, Node* parent, const char* name, const char* label, const char* request,
                         const char* args, float x, float y, float w)
{
    return b.Ensure<Ps1Button>(parent, name, [&](Ps1Button* btn) {
        Place(btn, x, y, w, 30.0f);
        btn->SetTextString(label);
        btn->SetRequest(request, args);
    });
}

Ps1Button* StepButton(Builder& b, Node* parent, const char* name, const char* label, const char* variable, int step,
                      float x, float y)
{
    return b.Ensure<Ps1Button>(parent, name, [&](Ps1Button* btn) {
        Place(btn, x, y, 36.0f, kRow + 2.0f);
        btn->SetTextString(label);
        btn->SetStep(variable, step, 1, 10);
    });
}

// Gamepad navigation over rows of buttons: up / down to the same column of the next row
// (or its last button), left / right within a row. Only links that are still empty are
// set, so links the user changed stay. Missing buttons (nullptr) are skipped.
void LinkNavigation(std::vector<std::vector<Button*>> rows)
{
    for (auto& row : rows)
    {
        row.erase(std::remove(row.begin(), row.end(), nullptr), row.end());
    }
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const std::vector<Button*>& r) { return r.empty(); }),
               rows.end());
    for (size_t r = 0; r < rows.size(); ++r)
    {
        for (size_t c = 0; c < rows[r].size(); ++c)
        {
            Button* btn = rows[r][c];
            if (c > 0 && btn->GetNavLeft() == nullptr) btn->SetNavLeft(rows[r][c - 1]);
            if (c + 1 < rows[r].size() && btn->GetNavRight() == nullptr) btn->SetNavRight(rows[r][c + 1]);
            if (r > 0 && btn->GetNavUp() == nullptr)
            {
                const std::vector<Button*>& up = rows[r - 1];
                btn->SetNavUp(up[c < up.size() ? c : up.size() - 1]);
            }
            if (r + 1 < rows.size() && btn->GetNavDown() == nullptr)
            {
                const std::vector<Button*>& down = rows[r + 1];
                btn->SetNavDown(down[c < down.size() ? c : down.size() - 1]);
            }
        }
    }
}

// Shows / hides the UI (HOME menu, or a gamepad button) and, for an interactive one,
// gives it the gamepad while open.
void Controller(Builder& b, Node* canvas, const char* title, bool startVisible, bool captureInput, Button* first)
{
    b.Ensure<Ps1MenuController>(canvas, "MenuController", [&](Ps1MenuController* c) {
        Place(c, 0.0f, 0.0f, 0.0f, 0.0f);
        c->Setup(title, startVisible, captureInput, first);
    });
}

// The UI's root Canvas: an existing one of that name anywhere in the scene is updated;
// otherwise a new one becomes the scene root (empty scene) or goes under the root.
Canvas* RootCanvas(Builder& b, const char* name)
{
    World* world = GetWorld(0);
    if (world == nullptr)
    {
        return nullptr;
    }
    Node* root = world->GetRootNode();
    if (root != nullptr)
    {
        Node* existing = (root->GetName() == name) ? root : root->FindChild(name, true);
        if (existing != nullptr)
        {
            ++b.kept;
            // by name: As<Canvas>() needs Canvas::ClassRuntimeId, which some editor import
            // libraries don't export
            Canvas* canvas = existing->Is("Canvas") ? static_cast<Canvas*>(existing) : nullptr;
            if (canvas == nullptr)
            {
                LogWarning("%s exists but is not a Canvas: left as it is", name);
            }
            return canvas;
        }
        Canvas* canvas = root->CreateChild<Canvas>(name);
        canvas->SetFullScreen();
        ++b.added;
        return canvas;
    }
    SharedPtr<Canvas> canvas = Node::Construct<Canvas>();
    canvas->SetName(name);
    canvas->SetFullScreen();
    world->SetRootNode(canvas.Get());
    ++b.added;
    return canvas.Get();
}

void Report(const Builder& b, const char* what)
{
    if (b.kept == 0)
    {
        LogDebug("%s created (%d nodes). Save it as a Scene to instance it.", what, b.added);
    }
    else
    {
        LogDebug("%s updated: %d node(s) added, %d existing left untouched.", what, b.added, b.kept);
    }
}

// ---- Digimon World ---------------------------------------------------------------------
void CreateDigimonStatsUI(void*)
{
    Builder b;
    Canvas* canvas = RootCanvas(b, "DigimonStatsUI");
    Quad* panel = b.Ensure<Quad>(canvas, "Panel", [](Quad* q) {
        Place(q, 16.0f, 16.0f, 340.0f, 590.0f);
        q->SetColor(kPanelColor);
    });

    Node* p = panel;
    Bound(b, p, "Name", "{partner_name}", 14.0f, 8.0f, 310.0f, 22.0f, kHeaderColor);
    Bound(b, p, "Species", "{partner_type>digimon_name}", 14.0f, 34.0f, 310.0f, kFontSize, kDimColor);

    Widget* vitals = Group(b, p, "Vitals", 14.0f, 64.0f, 312.0f, 56.0f);
    Label(b, vitals, "HPLabel", "HP", 0.0f, 0.0f, 36.0f);
    Bar(b, vitals, "HPBar", "hp", "hp_max", 36.0f, 0.0f, 170.0f, {0.35f, 0.8f, 0.35f, 1.0f});
    Bound(b, vitals, "HPValue", "{hp}/{hp_max}", 214.0f, 0.0f, 98.0f);
    Label(b, vitals, "MPLabel", "MP", 0.0f, 28.0f, 36.0f);
    Bar(b, vitals, "MPBar", "mp", "mp_max", 36.0f, 28.0f, 170.0f, {0.35f, 0.55f, 0.95f, 1.0f});
    Bound(b, vitals, "MPValue", "{mp}/{mp_max}", 214.0f, 28.0f, 98.0f);

    Widget* stats = Group(b, p, "Stats", 14.0f, 128.0f, 312.0f, 2 * kRow);
    Bound(b, stats, "OffDef", "Offense {offense}    Defense {defense}", 0.0f, 0.0f, 312.0f);
    Bound(b, stats, "SpdBrn", "Speed {speed}    Brains {brains}", 0.0f, kRow, 312.0f);

    Widget* care = Group(b, p, "Care", 14.0f, 184.0f, 312.0f, 6 * kRow);
    Bound(b, care, "Mood", "Happiness {happiness}    Discipline {discipline}", 0.0f, 0 * kRow, 312.0f);
    Bound(b, care, "Body", "Weight {weight}    Tiredness {tiredness}", 0.0f, 1 * kRow, 312.0f);
    Bound(b, care, "Record", "Care mistakes {care_mistakes}    Battles {battles}", 0.0f, 2 * kRow, 312.0f);
    Bound(b, care, "Life", "Age {age} days    Life {lifetime} h", 0.0f, 3 * kRow, 312.0f);
    Bound(b, care, "Health", "Virus {virus_bar}/16    Lives {lives}", 0.0f, 4 * kRow, 312.0f);
    Bound(b, care, "Money", "Bits {money}", 0.0f, 5 * kRow, 312.0f, kFontSize, kHeaderColor);

    Widget* clock = Group(b, p, "Clock", 14.0f, 336.0f, 312.0f, kRow);
    Bound(b, clock, "Time", "Day {day}    {hour:02}:{minute:02}", 0.0f, 0.0f, 312.0f, kFontSize, kDimColor);

    Widget* evo = Group(b, p, "Evolution", 14.0f, 370.0f, 312.0f, 8 * kRow + 16.0f);
    Label(b, evo, "Header", "Next evolutions (criteria met / 4)", 0.0f, 0.0f, 312.0f, kFontSize, kHeaderColor);
    Bound(b, evo, "NextCheck", "Next check in {evo_hours_left} h", 0.0f, kRow, 312.0f, kFontSize, kDimColor);
    for (int i = 0; i < 6; ++i)
    {
        char name[16], format[96];
        snprintf(name, sizeof(name), "Evo%d", i);
        snprintf(format, sizeof(format), "{next_evo[%d]>digimon_name}    {next_evo_score[%d]}/4", i, i);
        Bound(b, evo, name, format, 0.0f, (2 + i) * kRow + 4.0f, 312.0f, kFontSize, kTextColor, true);
    }
    // always shown; HOME menu > Hide Stats / Show Stats
    Controller(b, canvas, "Stats", true, false, nullptr);
    Report(b, "Digimon Stats UI");
}

void CreateDigimonCheatsUI(void*)
{
    Builder b;
    Canvas* canvas = RootCanvas(b, "DigimonCheatsUI");
    Quad* panel = b.Ensure<Quad>(canvas, "Panel", [](Quad* q) {
        // top right corner of the screen
        q->SetAnchorMode(AnchorMode::TopRight);
        q->SetPosition(-316.0f, 16.0f);
        q->SetDimensions(300.0f, 470.0f);
        q->SetColor(kPanelColor);
    });

    Node* p = panel;
    Label(b, p, "Title", "Cheats", 14.0f, 8.0f, 272.0f, 22.0f, kHeaderColor);

    std::vector<std::vector<Button*>> nav; // rows of buttons, for gamepad navigation

    Widget* toggles = Group(b, p, "Toggles", 14.0f, 42.0f, 272.0f, 6 * 30.0f);
    const struct { const char* id; const char* label; const char* variable; } kToggles[] = {
        {"InfiniteHP", "Infinite HP", "cheat_infhp"},
        {"AlwaysFull", "Always full", "cheat_full"},
        {"NoToilet", "Never needs the toilet", "cheat_nopoop"},
        {"NoPoopPenalty", "No poop penalty", "cheat_nopoopfine"},
        {"OneHitKO", "One hit KO", "cheat_ohko"},
        {"NeverMiss", "Never miss", "cheat_nevermiss"},
    };
    for (int i = 0; i < 6; ++i)
    {
        nav.push_back({ToggleButton(b, toggles, kToggles[i].id, kToggles[i].label, kToggles[i].variable, i * 30.0f)});
    }

    Widget* mults = Group(b, p, "Multipliers", 14.0f, 232.0f, 272.0f, 3 * 32.0f);
    const struct { const char* id; const char* format; const char* variable; } kMults[] = {
        {"Attack", "Attack x{cheat_atkmul}", "cheat_atkmul"},
        {"Gains", "Stat gains x{cheat_gainmul}", "cheat_gainmul"},
        {"Bits", "Bits x{cheat_bitsmul}", "cheat_bitsmul"},
    };
    for (int i = 0; i < 3; ++i)
    {
        const float y = i * 32.0f;
        Widget* row = Group(b, mults, kMults[i].id, 0.0f, y, 272.0f, 30.0f);
        Bound(b, row, "Value", kMults[i].format, 0.0f, 2.0f, 180.0f);
        nav.push_back({StepButton(b, row, "Minus", "-", kMults[i].variable, -1, 190.0f, 0.0f),
                       StepButton(b, row, "Plus", "+", kMults[i].variable, 1, 232.0f, 0.0f)});
    }

    Widget* actions = Group(b, p, "Actions", 14.0f, 342.0f, 272.0f, 3 * 38.0f);
    Label(b, actions, "Header", "Actions", 0.0f, 0.0f, 272.0f, kFontSize, kHeaderColor);
    nav.push_back({RequestButton(b, actions, "Heal", "Heal", "heal", "", 0.0f, 28.0f, 131.0f),
                   RequestButton(b, actions, "AddBits", "+1000 bits", "add_money", "1000", 141.0f, 28.0f, 131.0f)});
    nav.push_back({RequestButton(b, actions, "EvolveBest", "Evolve (best)", "evolve_good", "", 0.0f, 66.0f, 131.0f),
                   RequestButton(b, actions, "EvolveSukamon", "Evolve (Sukamon)", "evolve_bad", "", 141.0f, 66.0f,
                                 131.0f)});
    LinkNavigation(nav);

    // hidden until opened from the HOME menu (Show Cheats); takes the gamepad while open,
    // B closes it
    Button* first = nullptr;
    for (const auto& row : nav)
    {
        for (Button* btn : row)
        {
            if (btn != nullptr && first == nullptr) first = btn;
        }
    }
    Controller(b, canvas, "Cheats", false, true, first);
    Report(b, "Digimon Cheats UI");
}

// Start in the game pauses it (game.json option pausemenu=1) and sets the variable
// "paused": this UI follows it. Resume / B / Start close it and resume the game.
void CreateDigimonPauseUI(void*)
{
    Builder b;
    Canvas* canvas = RootCanvas(b, "DigimonPauseUI");
    Quad* panel = b.Ensure<Quad>(canvas, "Panel", [](Quad* q) {
        // middle of the screen
        q->SetAnchorMode(AnchorMode::Mid);
        q->SetPosition(-130.0f, -90.0f);
        q->SetDimensions(260.0f, 180.0f);
        q->SetColor(kPanelColor);
    });

    Node* p = panel;
    Label(b, p, "Title", "Paused", 14.0f, 8.0f, 232.0f, 22.0f, kHeaderColor);
    Bound(b, p, "Where", "{map>map_name}    Day {day}  {hour:02}:{minute:02}", 14.0f, 36.0f, 232.0f, kFontSize,
          kDimColor, true);

    Widget* actions = Group(b, p, "Actions", 14.0f, 72.0f, 232.0f, 2 * 40.0f);
    Button* resume = RequestButton(b, actions, "Resume", "Resume", "set paused", "0", 0.0f, 0.0f, 232.0f);
    Button* save = RequestButton(b, actions, "SaveGame", "Save Game", "save", "", 0.0f, 40.0f, 232.0f);
    LinkNavigation({{resume}, {save}});

    b.Ensure<Ps1MenuController>(canvas, "MenuController", [&](Ps1MenuController* c) {
        Place(c, 0.0f, 0.0f, 0.0f, 0.0f);
        c->Setup("Pause", false, true, resume);
        c->SetBoundVariable("paused");
        c->SetInHomeMenu(false); // the game opens it, not the HOME menu
    });
    Report(b, "Digimon Pause UI");
}
}

void Ps1GameUIs::RegisterMenus(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr || hooks->AddMenuItem == nullptr)
    {
        return;
    }
    // games translated into this addon (Source/Guest) get their UI tools
    if (ps1w_find_module("com.recomp.digimonworld") != nullptr)
    {
        hooks->AddMenuItem(hookId, "Tools", "Recomp/Digimon/Create Stats UI", CreateDigimonStatsUI, nullptr, nullptr);
        hooks->AddMenuItem(hookId, "Tools", "Recomp/Digimon/Create Cheats UI", CreateDigimonCheatsUI, nullptr, nullptr);
        hooks->AddMenuItem(hookId, "Tools", "Recomp/Digimon/Create Pause UI", CreateDigimonPauseUI, nullptr, nullptr);
    }
}

#endif
