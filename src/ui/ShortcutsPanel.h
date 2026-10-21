#pragma once

namespace materializr {

class KeyBindings;

class ShortcutsPanel {
public:
    ShortcutsPanel();
    void setVisible(bool vis);
    bool isVisible() const;
    // The live bindings the keyboard sections read, so the panel shows what the
    // user has actually configured (Settings > Navigation) rather than the
    // defaults. Not owned.
    void setBindings(const KeyBindings* b) { m_bindings = b; }
    void render();
private:
    bool m_visible = false;
    const KeyBindings* m_bindings = nullptr;
};

} // namespace materializr
