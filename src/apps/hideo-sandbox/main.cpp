#include <karm-sys/entry.h>
#include <karm-text/book.h>
#include <karm-text/family.h>
#include <karm-ui/app.h>

Async::Task<> entryPointAsync(Sys::Context &ctx) {
    Text::FontBook fontBook;
    co_try$(fontBook.loadAll());

    auto family = Text::FontFamily::make(fontBook)
                      .add({"Dancing Script"s})
                      .add({"Noto Emoji Medium"s})
                      .add({"Material Design Icons"s})
                      .bake();

    Text::ProseStyle style{
        .font = {family, 48},
        .align = Text::TextAlign::CENTER,
        .color = Gfx::BLACK,
    };

    co_return Ui::runApp(
        ctx,
        Ui::text(style, "🐢 Carle 󰳗 !") |
            Ui::center() |
            Ui::box({
                .padding = {16, 8},
                .borderRadii = {32, 8, 0, 0, 0, 0, 0, 0},
                .borderWidth = 10,
                .borderFill = Gfx::RED,
                .backgroundFill = Gfx::WHITE,
            })
    );
}
