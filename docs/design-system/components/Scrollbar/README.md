# Scrollbar

A thin, square scrollbar inside lists and the preview: a 12 px track in `input` with a 1 px `divider` edge, and a 6 px `border` thumb (3.5:1 on `input`) that turns `secondary` under the pointer.

- No arrow buttons and no rounded thumb.
- Lists scroll rather than overlap when the window is small, as the Linux main window already does.
- Keyboard: arrow keys move the selection and the list follows it; Page Up and Page Down move a page.

The consumer supplies the scroll range and position.

The preview is a static HTML rendition of these rules.
