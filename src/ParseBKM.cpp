/* Copyright 2020 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/BitManip.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/Log.h"

#include "wingui/WinGui.h"
#include "wingui/Layout.h"
#include "wingui/Window.h"
#include "wingui/TreeModel.h"
#include "wingui/TreeCtrl.h"

#include "EngineBase.h"
#include "ParseBKM.h"

/*
Creating and parsing of .bkm files that contain alternative bookmarks view
for PDF files.
*/

Bookmarks::~Bookmarks() {
    delete toc;
}

static void SerializeKeyVal(char* key, WCHAR* val, str::Str& s) {
    CrashIf(!key);
    if (!val) {
        return;
    }
    s.AppendFmt(" %s:", key);
    AutoFree str = strconv::WstrToUtf8(val);
    sv::AppendQuotedString(str.as_view(), s);
}

static void SerializeDest(PageDestination* dest, str::Str& s) {
    if (!dest) {
        return;
    }
    s.AppendFmt(" destkind:%s", dest->kind);
    SerializeKeyVal("destname", dest->GetName(), s);
    SerializeKeyVal("destvalue", dest->GetValue(), s);
    if (dest->pageNo > 0) {
        s.AppendFmt(" destpage:%d", dest->pageNo);
    }
    RectD r = dest->rect;
    if (!r.empty()) {
        s.AppendFmt(" destrect:%f,%f,%f,%f", r.x, r.y, r.dx, r.dy);
    }
}

static void SerializeBookmarksRec(DocTocItem* node, int level, str::Str& s) {
    if (level == 0) {
        s.Append("title: default view\n");
    }

    while (node) {
        for (int i = 0; i < level; i++) {
            s.Append("  ");
        }
        WCHAR* title = node->Text();
        AutoFree titleA = strconv::WstrToUtf8(title);
        sv::AppendQuotedString(titleA.as_view(), s);
        auto flags = node->fontFlags;
        if (bit::IsSet(flags, fontBitItalic)) {
            s.Append(" font:italic");
        }
        if (bit::IsSet(flags, fontBitBold)) {
            s.Append(" font:bold");
        }
        if (node->color != ColorUnset) {
            s.Append(" ");
            SerializeColor(node->color, s);
        }
        if (node->pageNo != 0) {
            s.AppendFmt(" page:%d", node->pageNo);
        }
        if (node->isOpenDefault) {
            s.AppendFmt(" open-default");
        }
        if (node->isOpenDefault) {
            s.AppendFmt(" open-toggled");
        }
        if (node->isUnchecked) {
            s.AppendFmt("  unchecked");
        }

        CrashIf(!node->PageNumbersMatch());
        SerializeDest(node->GetPageDestination(), s);
        s.Append("\n");

        SerializeBookmarksRec(node->child, level + 1, s);
        node = node->next;
    }
}

// parses "quoted string"
static str::Str parseLineTitle(std::string_view& sv) {
    str::Str res;
    size_t n = sv.size();
    // must be at least: ""
    if (n < 2) {
        return res;
    }
    const char* s = sv.data();
    const char* e = s + n;
    if (s[0] != '"') {
        return res;
    }
    s++;
    while (s < e) {
        char c = *s;
        if (c == '"') {
            // the end
            sv::SkipTo(sv, s + 1);
            return res;
        }
        if (c != '\\') {
            res.AppendChar(c);
            s++;
            continue;
        }
        // potentially un-escape
        s++;
        if (s >= e) {
            break;
        }
        char c2 = *s;
        bool unEscape = (c2 == '\\') || (c2 == '"');
        if (!unEscape) {
            res.AppendChar(c);
            continue;
        }
        res.AppendChar(c2);
        s++;
    }

    return res;
}

static std::tuple<COLORREF, bool> parseColor(std::string_view sv) {
    COLORREF c = 0;
    bool ok = ParseColor(&c, sv);
    return {c, ok};
}

#if 0
static void SerializeDest(PageDestination* dest, str::Str& s) {
    if (!dest) {
        return;
    }
    s.AppendFmt(" destkind:%s", dest->kind);
    SerializeKeyVal("destname", dest->GetName(), s);
    SerializeKeyVal("destvalue", dest->GetValue(), s);
    if (dest->pageNo > 0) {
        s.AppendFmt(" destpage:%d", dest->pageNo);
    }
    RectD r = dest->rect;
    if (!r.empty()) {
        s.AppendFmt(" destrect:%f,%f,%f,%f", r.x, r.y, r.dx, r.dy);
    }
}
#endif

struct ParsedDest {
    int pageNo;
};

std::tuple<ParsedDest*, bool> parseDestination(std::string_view& sv) {
    if (!str::StartsWith(sv, "page:")) {
        return {nullptr, false};
    }
    // TODO: actually parse the info
    auto* res = new ParsedDest{1};
    return {res, true};
}

struct parsedKV {
    char* key = nullptr;
    char* val = nullptr;
    bool ok = false;

    parsedKV() = default;
    ~parsedKV();
};

parsedKV::~parsedKV() {
    free(key);
    free(val);
}

// line could be:
// "key"
// "key:unquoted-value"
// "key:"quoted value"
// updates str in place to account for parsed data
parsedKV parseKV(std::string_view& line) {
    // eat white space
    sv::SkipChars(line, ' ');

    // find end of key (':', ' ' or end of text)
    const char* s = line.data();
    const char* end = s + line.length();

    char c;
    const char* key = s;
    const char* keyEnd = nullptr;
    str::Str val;
    while (s < end) {
        c = *s;
        if (c == ':' || c == ' ') {
            keyEnd = s - 1;
            break;
        }
        s++;
    }
    if (keyEnd == nullptr) {
        keyEnd = s;
    } else {
        if (*s == ':') {
            s++;
        }
    }
    parsedKV res;
    return res;
}

// a single line in .bmk file is:
// indentation "quoted title" additional-metadata* destination
static DocTocItem* parseBookmarksLine(std::string_view line, size_t* indentOut) {
    auto origLine = line; // save for debugging

    // lines might start with an indentation, 2 spaces for one level
    // TODO: maybe also count tabs as one level?
    size_t indent = sv::SkipChars(line, ' ');
    // must be multiple of 2
    if (indent % 2 != 0) {
        return nullptr;
    }
    *indentOut = indent / 2;
    sv::SkipChars(line, ' ');
    // TODO: no way to indicate an error
    str::Str title = parseLineTitle(line);
    DocTocItem* res = new DocTocItem();
    res->title = strconv::Utf8ToWstr(title.AsView());
    PageDestination* dest = nullptr;

    // parse meta-data and page destination
    std::string_view part;
    while (line.size() > 0) {
        part = sv::ParseUntil(line, ' ');

        if (str::Eq(part, "font:bold")) {
            bit::Set(res->fontFlags, fontBitBold);
            continue;
        }

        if (str::Eq(part, "font:italic")) {
            bit::Set(res->fontFlags, fontBitItalic);
            continue;
        }

        auto [color, ok] = parseColor(part);
        if (ok) {
            res->color = color;
            continue;
        }

        if (str::EqI(part, "open-default")) {
            res->isOpenDefault = true;
            continue;
        }
        if (str::EqI(part, "open-toggled")) {
            res->isOpenToggled = true;
            continue;
        }
        if (str::Eq(part, "unchecked")) {
            res->isUnchecked = true;
            continue;
        }
    }
    return res;
}

struct DocTocItemWithIndent {
    DocTocItem* item = nullptr;
    size_t indent = 0;

    DocTocItemWithIndent() = default;
    DocTocItemWithIndent(DocTocItem* item, size_t indent);
    ~DocTocItemWithIndent() = default;
};

DocTocItemWithIndent::DocTocItemWithIndent(DocTocItem* item, size_t indent) {
    this->item = item;
    this->indent = indent;
}

// TODO: read more than one
static bool parseBookmarks(std::string_view sv, Vec<Bookmarks*>* bkms) {
    Vec<DocTocItemWithIndent> items;

    // first line should be "file: $file"
    auto line = sv::ParseUntil(sv, '\n');
    auto file = sv::ParseKV(line, "file");
    if (file.empty()) {
        return false;
    }

    // this line should be "title: $title"
    line = sv::ParseUntil(sv, '\n');
    auto title = sv::ParseKV(line, "title");
    if (title.empty()) {
        return false;
    }
    auto tree = new DocTocTree();
    tree->name = str::Dup(title);
    size_t indent = 0;
    while (true) {
        line = sv::ParseUntil(sv, '\n');
        if (line.empty()) {
            break;
        }
        auto* item = parseBookmarksLine(line, &indent);
        if (item == nullptr) {
            for (auto& el : items) {
                delete el.item;
            }
            delete tree;
            return false;
        }
        DocTocItemWithIndent iwl = {item, indent};
        items.Append(iwl);
    }
    size_t nItems = items.size();
    if (nItems == 0) {
        for (auto& el : items) {
            delete el.item;
        }
        delete tree;
        return false;
    }

    tree->root = items[0].item;

    /* We want to reconstruct tree from array
        a
         b1
         b2
        a2
         b3
\    */
    for (size_t i = 1; i < nItems; i++) {
        const auto& curr = items[i];
        auto& prev = items[i - 1];
        auto item = curr.item;
        if (prev.indent == curr.indent) {
            // b1 -> b2
            prev.item->next = item;
        } else if (curr.indent > prev.indent) {
            // a2 -> b3
            prev.item->child = item;
        } else {
            // a -> a2
            bool didFound = false;
            for (int j = (int)i - 1; j >= 0; j--) {
                prev = items[j];
                if (prev.indent == curr.indent) {
                    prev.item->AddSibling(item);
                    didFound = true;
                    break;
                }
            }
            if (!didFound) {
                tree->root->AddSibling(item);
            }
        }
    }

    auto* bkm = new Bookmarks();
    bkm->toc = tree;
    bkms->Append(bkm);

    return true;
}

bool ParseBookmarksFile(std::string_view path, Vec<Bookmarks*>* bkms) {
    AutoFree d = file::ReadFile(path);
    if (!d.data) {
        return false;
    }
    return parseBookmarks(d.as_view(), bkms);
}

Vec<Bookmarks*>* LoadAlterenativeBookmarks(std::string_view baseFileName) {
    str::Str path = baseFileName;
    path.Append(".bkm");

    auto* res = new Vec<Bookmarks*>();

    auto ok = ParseBookmarksFile(path.AsView(), res);
    if (!ok) {
        DeleteVecMembers(*res);
        delete res;
        return nullptr;
    }

    // TODO: read more than one
    return res;
}

bool ExportBookmarksToFile(const Vec<Bookmarks*>& bookmarks, const char* bkmPath) {
    str::Str s;
    for (auto&& bkm : bookmarks) {
        DocTocTree* tocTree = bkm->toc;
        const char* path = tocTree->filePath;
        s.AppendFmt("file: %s\n", path);
        SerializeBookmarksRec(tocTree->root, 0, s);
        // dbglogf("%s\n", s.Get());
    }
    return file::WriteFile(bkmPath, s.as_view());
}
