// editor.hh — structured 2D ("natural display") math input editor.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// A small tree the keypad edits in place: a Row is a horizontal sequence of
// Nodes; an Atom is a run of literal canonical chars (a digit, an operator,
// "sin(", "pi", …); a template (Frac/Sqrt/NthRoot/Power) owns child Rows (its
// "slots") that the user fills via the arrow keys. A Cursor marks an insertion
// point inside one Row. The tree linearizes back to the canonical ASCII the
// existing lexer/parser/evaluator already accept, so the math core is untouched.
//
// UI- and Vulkan-free (pure C++17): the renderer walks root()/cursor() read-only.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace calcedit {

struct Node;

// A horizontal sequence of nodes. Rows live either as the editor root or inside
// a Node's `slots` vector (which is sized once and never resized), so a Row's
// address is stable for the lifetime of the tree and the Cursor may hold a Row*.
struct Row {
    std::vector<std::unique_ptr<Node>> items;
    Node* parentNode = nullptr;  // node owning this row as a slot; null for root
    int   slotIndex  = 0;        // which slot of parentNode this row is
};

struct Node {
    enum Kind { Atom, Frac, Sqrt, NthRoot, Power };
    Kind        kind = Atom;
    std::string text;            // Atom: the literal canonical chars
    std::vector<Row> slots;      // Frac{num,den} Sqrt{rad} NthRoot{index,rad} Power{exp}
    Row*        parentRow = nullptr;  // row containing this node
};

// Insertion point: between items[index-1] and items[index] of `row`.
struct Cursor {
    Row* row   = nullptr;
    int  index = 0;
};

class Editor {
public:
    Editor() { cursor_.row = &root_; cursor_.index = 0; }
    Editor(const Editor&)            = delete;  // root_/cursor_ self-reference
    Editor& operator=(const Editor&) = delete;

    void clear();

    // Replace the whole expression with `s` (canonical ASCII), as a flat row of
    // atoms that linearize()s back to `s` exactly. Runs of letters become one
    // atom ("sin", "pi", "x"); every other char is its own atom (so digits and
    // operators backspace one at a time). Used to load a stored/seeded equation
    // into the editor for editing. 2D templates appear only when the user inserts
    // them; imported text stays linear.
    void loadString(const std::string& s);

    // Insert a literal run (digit/operator/"."/"("/")"/"sin("/"pi"/…). Returns
    // false WITHOUT inserting if it would create an implicit multiplication (a
    // value-token landing directly after another value-token) — the editor forbids
    // implicit mult, so the user must type an explicit '·' (x then x is blocked).
    // Operators and digits/decimals continuing a number are always allowed.
    bool insertAtom(const std::string& text);
    // Insert a literal run UNCONDITIONALLY (no implicit-mult guard). For loading
    // stored/seeded text and programmatic insertion (e.g. the Ans value).
    void insertLiteral(const std::string& text);
    // Insert a template with empty slots; the cursor moves into the slot the user
    // types first. Frac/Sqrt/NthRoot start a value, so they return false (no insert)
    // when that would be an implicit mult.
    bool insertFraction();
    bool insertSqrt();
    bool insertNthRoot();
    // Power attaches to the preceding sibling; it returns false (no insert) when
    // there is no value to its left to serve as the base (row start, after an
    // operator or an open paren) — a superscript must never float baseless.
    bool insertPower();

    void backspace();

    void moveLeft();
    void moveRight();
    void moveUp();
    void moveDown();

    // Canonical ASCII for the evaluator (empty slots emit nothing → the parser
    // reports a Syntax Error, matching today's behavior for malformed input).
    std::string linearize() const;

    const Row&    root()   const { return root_; }
    const Cursor& cursor() const { return cursor_; }

    bool empty()      const { return root_.items.empty(); }
    bool isLoneZero() const;        // root is exactly a single Atom "0"
    bool cursorAtRoot() const { return cursor_.row == &root_; }

private:
    Row    root_;
    Cursor cursor_;

    // Create a template/atom node with `nslots` empty slots, splice it into the
    // current row at the cursor, and return it (cursor is NOT advanced).
    Node* insertNode(Node::Kind kind, int nslots);

    // True if a token whose first canonical char is `newFirst` would sit directly
    // after a value-ending token at the cursor — i.e. inserting it is an implicit
    // multiplication (forbidden). Digit/'.'-runs and letter-runs that merge into the
    // same number/identifier are NOT implicit mult.
    bool implicitMulBefore(char newFirst) const;

    static int  indexOfNode(const Row* row, const Node* n);
    static void linearizeRow(const Row& row, std::string& out);
    static void linearizeNode(const Node& n, std::string& out);
};

}  // namespace calcedit
