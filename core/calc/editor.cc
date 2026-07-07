// editor.cc — structured 2D math input editor.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "editor.hh"

#include <algorithm>
#include <cctype>

namespace calcedit {

namespace {

// Slot a left-to-right entry lands in when the cursor steps INTO a template
// from its left edge (start of that slot). All templates expose slot 0 first.
int entrySlotFromLeft(Node::Kind) { return 0; }

// Templates whose two slots chain horizontally: every slot is visited by
// left/right traversal (and so by DEL), in slot order. Frac walks
// numerator → denominator; NthRoot walks index → radicand.
bool twoSlotChain(Node::Kind k) { return k == Node::Frac || k == Node::NthRoot; }

// Slot the cursor lands in (at its END) when stepping into a template from its
// right edge: the LAST horizontal slot (denominator / radicand).
int entrySlotFromRight(Node::Kind k) { return twoSlotChain(k) ? 1 : 0; }

// Horizontal slot chaining: moving right at the end of slot `s` crosses into
// this sibling slot if >= 0, otherwise exits the node to the right.
int rightSiblingSlot(Node::Kind k, int s) {
    return (twoSlotChain(k) && s == 0) ? 1 : -1;  // num → den; index → radicand
}
// Moving left at the start of slot `s` crosses into this sibling (at its end).
int leftSiblingSlot(Node::Kind k, int s) {
    return (twoSlotChain(k) && s == 1) ? 0 : -1;  // den → num; radicand → index
}

// True when every slot of a template is empty (nothing left to protect).
bool slotsEmpty(const Node& n) {
    for (const auto& sl : n.slots)
        if (!sl.items.empty()) return false;
    return true;
}

}  // namespace

void Editor::clear() {
    root_.items.clear();
    cursor_.row   = &root_;
    cursor_.index = 0;
}

Node* Editor::insertNode(Node::Kind kind, int nslots) {
    auto node = std::make_unique<Node>();
    node->kind = kind;
    node->slots.resize(static_cast<size_t>(nslots));
    Node* raw = node.get();
    for (int i = 0; i < nslots; ++i) {
        raw->slots[i].parentNode = raw;
        raw->slots[i].slotIndex  = i;
    }
    raw->parentRow = cursor_.row;
    cursor_.row->items.insert(cursor_.row->items.begin() + cursor_.index,
                              std::move(node));
    return raw;
}

bool Editor::implicitMulBefore(char newFirst) const {
    if (cursor_.index <= 0) return false;                 // nothing to the left
    const Node* prev = cursor_.row->items[cursor_.index - 1].get();
    char prevLast;
    if (prev->kind == Node::Atom) {
        if (prev->text.empty()) return false;
        prevLast = prev->text.back();
    } else {
        prevLast = ')';  // Frac/Sqrt/NthRoot/Power all linearize ending in ')'
    }
    auto alnum     = [](char c){ return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    auto endsVal   = [&](char c){ return alnum(c) || c == '.' || c == ')'; };
    auto startsVal = [&](char c){ return alnum(c) || c == '.' || c == '('; };
    if (!endsVal(prevLast) || !startsVal(newFirst)) return false;   // e.g. "sin(" then x
    // Only digit runs merge into one token (7,7 → 77). Letters do NOT: the keypad
    // inserts whole tokens ("x", "pi", "sin("), so a letter landing after a letter
    // is always a product (x·x) and must be typed with an explicit '·'.
    bool numMerge = (std::isdigit((unsigned char)prevLast) || prevLast == '.') &&
                    (std::isdigit((unsigned char)newFirst) || newFirst == '.');   // 7,7 → 77
    return !numMerge;
}

void Editor::insertLiteral(const std::string& text) {
    Node* n = insertNode(Node::Atom, 0);
    n->text = text;
    cursor_.index += 1;  // step past the freshly inserted atom
}

bool Editor::insertAtom(const std::string& text) {
    if (!text.empty() && implicitMulBefore(text[0])) return false;  // forbid implicit ·
    insertLiteral(text);
    return true;
}

void Editor::loadString(const std::string& s) {
    clear();
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (std::isalpha(ch)) {
            size_t j = i;
            while (j < n && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            insertLiteral(s.substr(i, j - i));  // "sin", "pi", "x", … (no guard: trusted text)
            i = j;
        } else {
            insertLiteral(std::string(1, s[i]));
            ++i;
        }
    }
}

bool Editor::insertFraction() {
    if (implicitMulBefore('(')) return false;
    Node* n = insertNode(Node::Frac, 2);
    cursor_.row = &n->slots[0];  // numerator
    cursor_.index = 0;
    return true;
}
bool Editor::insertSqrt() {
    if (implicitMulBefore('s')) return false;   // linearizes to "sqrt(…)"
    Node* n = insertNode(Node::Sqrt, 1);
    cursor_.row = &n->slots[0];  // radicand
    cursor_.index = 0;
    return true;
}
bool Editor::insertNthRoot() {
    if (implicitMulBefore('(')) return false;
    Node* n = insertNode(Node::NthRoot, 2);
    cursor_.row = &n->slots[0];  // index (degree), typed first
    cursor_.index = 0;
    return true;
}
bool Editor::insertPower() {     // attaches to the preceding sibling → never implicit mult,
                                 // but that sibling must exist and end a value (the base)
    if (cursor_.index <= 0) return false;                 // row start: nothing to raise
    const Node* prev = cursor_.row->items[cursor_.index - 1].get();
    char prevLast = ')';         // Frac/Sqrt/NthRoot/Power all linearize ending in ')'
    if (prev->kind == Node::Atom) {
        if (prev->text.empty()) return false;
        prevLast = prev->text.back();
    }
    if (!(std::isalnum(static_cast<unsigned char>(prevLast)) ||
          prevLast == '.' || prevLast == ')'))
        return false;            // after "+", "sin(", "(" … there is no base
    Node* n = insertNode(Node::Power, 1);
    cursor_.row = &n->slots[0];  // exponent
    cursor_.index = 0;
    return true;
}

int Editor::indexOfNode(const Row* row, const Node* n) {
    for (int i = 0; i < static_cast<int>(row->items.size()); ++i)
        if (row->items[i].get() == n) return i;
    return -1;
}

void Editor::moveRight() {
    Row* row = cursor_.row;
    int  i   = cursor_.index;
    if (i < static_cast<int>(row->items.size())) {
        Node* n = row->items[i].get();
        if (n->kind == Node::Atom) {
            cursor_.index = i + 1;
        } else {  // enter the template from the left
            int s = entrySlotFromLeft(n->kind);
            cursor_.row = &n->slots[s];
            cursor_.index = 0;
        }
        return;
    }
    // At the end of the row: cross into a sibling slot or exit the node.
    if (!row->parentNode) return;  // root: nowhere further right
    Node* P  = row->parentNode;
    int   ns = rightSiblingSlot(P->kind, row->slotIndex);
    if (ns >= 0) {
        cursor_.row = &P->slots[ns];
        cursor_.index = 0;
    } else {
        Row* pr = P->parentRow;
        cursor_.row = pr;
        cursor_.index = indexOfNode(pr, P) + 1;
    }
}

void Editor::moveLeft() {
    Row* row = cursor_.row;
    int  i   = cursor_.index;
    if (i > 0) {
        Node* n = row->items[i - 1].get();
        if (n->kind == Node::Atom) {
            cursor_.index = i - 1;
        } else {  // enter the template from the right (land at slot end)
            int s = entrySlotFromRight(n->kind);
            cursor_.row = &n->slots[s];
            cursor_.index = static_cast<int>(n->slots[s].items.size());
        }
        return;
    }
    if (!row->parentNode) return;  // root: nowhere further left
    Node* P  = row->parentNode;
    int   ps = leftSiblingSlot(P->kind, row->slotIndex);
    if (ps >= 0) {
        cursor_.row = &P->slots[ps];
        cursor_.index = static_cast<int>(P->slots[ps].items.size());
    } else {
        Row* pr = P->parentRow;
        cursor_.row = pr;
        cursor_.index = indexOfNode(pr, P);
    }
}

void Editor::moveDown() {
    Row* row = cursor_.row;
    if (row->parentNode && row->parentNode->kind == Node::Frac &&
        row->slotIndex == 0) {
        Row* den = &row->parentNode->slots[1];
        cursor_.row = den;
        cursor_.index = std::min(cursor_.index, static_cast<int>(den->items.size()));
    }
}

void Editor::moveUp() {
    Row* row = cursor_.row;
    if (row->parentNode && row->parentNode->kind == Node::Frac &&
        row->slotIndex == 1) {
        Row* num = &row->parentNode->slots[0];
        cursor_.row = num;
        cursor_.index = std::min(cursor_.index, static_cast<int>(num->items.size()));
    }
}

// DEL's invariant: every press either deletes something or moves the cursor
// deeper-right INTO content — it never jumps left past content it hasn't
// consumed (which would strand the cursor with undeletable material to its
// right). Holding DEL therefore always erases the whole expression.
void Editor::backspace() {
    Row* row = cursor_.row;
    int  i   = cursor_.index;
    if (i > 0) {
        Node* n = row->items[i - 1].get();
        if (n->kind == Node::Atom) {
            row->items.erase(row->items.begin() + (i - 1));
            cursor_.index = i - 1;
        } else if (slotsEmpty(*n)) {
            // An empty template deletes in one press.
            row->items.erase(row->items.begin() + (i - 1));
            cursor_.index = i - 1;
        } else {
            // Step into a filled template instead of nuking it in one press
            // (landing at the end of its LAST horizontal slot, so the whole
            // content gets consumed slot by slot).
            int s = entrySlotFromRight(n->kind);
            cursor_.row = &n->slots[s];
            cursor_.index = static_cast<int>(n->slots[s].items.size());
        }
        return;
    }
    // At the start of a slot.
    if (!row->parentNode) return;  // root start: nothing to delete
    Node* P  = row->parentNode;
    Row*  pr = P->parentRow;
    int   pi = indexOfNode(pr, P);
    if (slotsEmpty(*P)) {          // hollow template → delete it
        cursor_.row = pr;
        cursor_.index = pi;
        pr->items.erase(pr->items.begin() + pi);
        return;
    }
    int ps = leftSiblingSlot(P->kind, row->slotIndex);
    if (ps >= 0) {                 // cross into the previous slot's end
        cursor_.row = &P->slots[ps];
        cursor_.index = static_cast<int>(P->slots[ps].items.size());
        return;
    }
    // Start of the FIRST slot with content still in the template: UNWRAP —
    // delete the structure but keep its contents, splicing every slot's items
    // (in slot order) into the parent row where the node stood. The cursor
    // lands before the spliced content: everything left of it was already
    // consumed, so DEL keeps working as expected (8/5 → 85, 2^(5) → 25).
    std::vector<std::unique_ptr<Node>> spliced;
    for (Row& sl : P->slots)
        for (auto& item : sl.items) spliced.push_back(std::move(item));
    cursor_.row = pr;
    cursor_.index = pi;
    pr->items.erase(pr->items.begin() + pi);           // destroys P (slots now empty)
    for (int k = 0; k < static_cast<int>(spliced.size()); ++k) {
        spliced[k]->parentRow = pr;
        pr->items.insert(pr->items.begin() + pi + k, std::move(spliced[k]));
    }
}

void Editor::linearizeNode(const Node& n, std::string& out) {
    switch (n.kind) {
        case Node::Atom: out += n.text; break;
        case Node::Frac:
            out += '(';  linearizeRow(n.slots[0], out);
            out += ")/("; linearizeRow(n.slots[1], out); out += ')';
            break;
        case Node::Sqrt:
            out += "sqrt("; linearizeRow(n.slots[0], out); out += ')';
            break;
        case Node::NthRoot:  // radicand ^ (1 / index)
            out += '(';  linearizeRow(n.slots[1], out);
            out += ")^(1/("; linearizeRow(n.slots[0], out); out += "))";
            break;
        case Node::Power:    // attaches to the preceding sibling in the row
            out += "^("; linearizeRow(n.slots[0], out); out += ')';
            break;
    }
}

void Editor::linearizeRow(const Row& row, std::string& out) {
    for (const auto& n : row.items) linearizeNode(*n, out);
}

std::string Editor::linearize() const {
    std::string out;
    linearizeRow(root_, out);
    return out;
}

bool Editor::isLoneZero() const {
    return root_.items.size() == 1 && root_.items[0]->kind == Node::Atom &&
           root_.items[0]->text == "0";
}

}  // namespace calcedit
