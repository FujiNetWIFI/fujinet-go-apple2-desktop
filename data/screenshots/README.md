# Screenshots

AppStream screenshots for the software-centre listings. These are referenced
by **absolute URL** from the metainfo, not installed with the app, so an image
only appears once it is committed *and tagged* -- the URL is what
GNOME Software and Flathub fetch.

    https://raw.githubusercontent.com/FujiNetWIFI/fujinet-go-apple2-desktop/v<version>/data/screenshots/<subdir>/<file>

## Layout

    data/screenshots/*.png       GNOME frontend  -> gnome metainfo
    data/screenshots/kde/*.png   KDE frontend    -> kde metainfo

Each frontend lists its own shots: they are separate AppStream components
with separate store pages, so a KDE listing showing the GTK UI would be
wrong.

## Conventions

* **Width 1600px max.** Flathub scales anything larger unfavourably in the
  store card. Downscale before committing:

      magick shot.png -resize 1600x\> -strip shot.png

* **Numbered by display order** (`1_...`, `2_...`); the first one listed in
  the metainfo is marked `type="default"` and becomes the card image.
* **PNG**, no alpha channel needed, `-strip` to drop EXIF.
* Give each one a `<caption>` in the metainfo -- it shows under the image.

## After adding

Add a `<screenshot>` block to the matching
`frontends/<frontend>/data/*.metainfo.xml`, then check both the structure and
the URLs:

    ctest --test-dir build-all -R metainfo     # offline, structural
    appstreamcli validate --explain frontends/kde/data/*.metainfo.xml

The second one fetches every URL, so run it *after* pushing; before that it
reports `screenshot-image-not-found`, which is expected rather than a fault
in the markup.
