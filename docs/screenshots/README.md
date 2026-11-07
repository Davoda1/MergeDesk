# Application captures

The comparison screenshots in this folder were captured from the actual
MergeDesk 1.0.0 GNU/Linux Qt application. Qt's offscreen platform
captures the widget content without an operating-system window frame.

The text, folder, table and image inputs are small demonstration fixtures.
They are not benchmark reports or claims of test results. Both themes use
the application's real colors, controls and comparison highlights.

The README banner is separate artwork in `docs/images/mergedesk-banner.png`;
it is not an application screenshot.

An offline preview can be generated from these actual PNG captures using
`packaging/create_preview.py`. From the repository root:

```sh
python3 packaging/create_preview.py docs/screenshots build/mergedesk-preview.html
```

The preview embeds the original images and their hashes and offers mode/theme
selection without loading external assets. It displays captures, not an
interactive implementation of the application.

The README text-comparison images and the About capture were refreshed on
2026-11-07 using Qt 6.11.3; the other captures were made on 2026-11-06.
Their sample settings files contain only synthetic demonstration
preferences. The separate banner illustration is not used as evidence of
application behavior.
