#pragma once

#include "flow.h"

namespace Vaev::Layout {

struct GridFlow : public Flow {
    static constexpr auto TYPE = GRID;

    using Flow::Flow;

    struct Cell {
        usize frag;
    };

    struct Dim {
    };

    Vec<Cell> _cells;
    Vec<Dim> _rows;
    Vec<Dim> _cols;

    Type type() const override {
        return TYPE;
    }

    void _clear() {
        _cells.clear();
        _rows.clear();
        _cols.clear();
    }

    void _createCells() {
        for (usize i = 0; i < _frags.len(); i++)
            _cells.pushBack({i});
    }

    void _sortByOrder() {
        stableSort(_cells, [&](auto a, auto b) {
            return styleAt(a.frag).order < styleAt(b.frag).order;
        });
    }

    void placeChildren(Context &ctx, Box box) override {
        Flow::placeChildren(ctx, box);

        _clear();
        _createCells();
        _sortByOrder();
    }
};

} // namespace Vaev::Layout
