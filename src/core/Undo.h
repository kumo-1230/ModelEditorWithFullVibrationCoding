#pragma once
// スナップショット方式の Undo/Redo。
// テクスチャのピクセルは shared_ptr で共有されるのでコピーは軽い。
#include "Scene.h"
#include <deque>
#include <string>

class UndoStack {
public:
    struct Entry {
        Scene scene;
        std::string label;
    };

    // 変更「前」の状態を積む
    void Push(const Scene& before, const std::string& label) {
        m_undo.push_back({ before, label });
        while (m_undo.size() > m_maxEntries) m_undo.pop_front();
        m_redo.clear();
    }
    bool Undo(Scene& current) {
        if (m_undo.empty()) return false;
        m_redo.push_back({ current, m_undo.back().label });
        current = std::move(m_undo.back().scene);
        m_undo.pop_back();
        return true;
    }
    bool Redo(Scene& current) {
        if (m_redo.empty()) return false;
        m_undo.push_back({ current, m_redo.back().label });
        current = std::move(m_redo.back().scene);
        m_redo.pop_back();
        return true;
    }
    void Clear() { m_undo.clear(); m_redo.clear(); }
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    const std::deque<Entry>& UndoEntries() const { return m_undo; }
    const std::deque<Entry>& RedoEntries() const { return m_redo; }
    size_t MaxEntries() const { return m_maxEntries; }
    void SetMaxEntries(size_t n) { m_maxEntries = n < 1 ? 1 : n; }

private:
    std::deque<Entry> m_undo, m_redo;
    size_t m_maxEntries = 40;
};
