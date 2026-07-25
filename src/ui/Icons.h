#pragma once

// Font Awesome 6 Free (Solid) icon codepoints, encoded as UTF-8 string
// literals so they can be concatenated straight into ImGui labels, e.g.
//   ImGui::Text(ICON_FA_FOLDER "  %s", name);
// The matching glyphs are merged into the font atlas from fa-solid-900.ttf.

// Glyph range to load from the icon font (covers every icon used below).
#define ICON_MIN_FA 0xf000
#define ICON_MAX_FA 0xf8ff

#define ICON_FA_FOLDER      "\xef\x81\xbb" // f07b  folder
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc" // f07c  folder-open
#define ICON_FA_FILE        "\xef\x85\x9b" // f15b  file
#define ICON_FA_FILM        "\xef\x80\x88" // f008  film   (scene)
#define ICON_FA_CUBE        "\xef\x86\xb2" // f1b2  cube   (object)
#define ICON_FA_IMAGE       "\xef\x80\xbe" // f03e  image
#define ICON_FA_MUSIC       "\xef\x80\x81" // f001  music
#define ICON_FA_PLUS        "\xef\x81\xa7" // f067  plus
#define ICON_FA_TRASH       "\xef\x87\xb8" // f1f8  trash
#define ICON_FA_FLOPPY_DISK "\xef\x83\x87" // f0c7  floppy (save)
#define ICON_FA_CLOCK       "\xef\x80\x97" // f017  clock  (recent)
#define ICON_FA_ARROW_DOWN  "\xef\x81\xa3" // f063  arrow-down (auto-scroll)
#define ICON_FA_CODE_BRANCH "\xef\x84\xa6" // f126  code-branch (git)
#define ICON_FA_PLAY        "\xef\x81\x8b" // f04b  play
#define ICON_FA_PAUSE       "\xef\x81\x8c" // f04c  pause
#define ICON_FA_ROTATE      "\xef\x80\x9e" // f01e  rotate
#define ICON_FA_HAMMER      "\xef\x9b\xa3" // f6e3  hammer
