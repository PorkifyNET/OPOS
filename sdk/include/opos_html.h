/*
 * HTML parser for OPOS applications.
 *
 * Builds a document tree from HTML text of any quality: unclosed and
 * misnested tags, missing <html>/<head>/<body>, character references
 * (&amp; &#169; &#xA9; and the common named ones), comments, doctypes and
 * the contents of <script>, <style>, <title> and <textarea>. It follows the
 * main ideas of the HTML5 tree builder (implied end tags, a few reopened
 * formatting elements) but not every rule: tables get no implied <tbody>,
 * and there is no foster parenting or form pointer.
 *
 * All text in the tree is UTF-8. Input is taken as UTF-8 unless a charset
 * name says "iso-8859-1" or "windows-1252" (the caller's hint, or a <meta>
 * tag in the first 1024 bytes); those bytes are converted.
 *
 * The tree lives in one arena and goes away with opos_html_free.
 *
 *   struct opos_html_doc *doc = opos_html_parse(data, length, "utf-8");
 *   for (node = doc->body->first_child; node; node = node->next_sibling) ...
 *   opos_html_free(doc);
 */
#ifndef OPOS_HTML_H
#define OPOS_HTML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum opos_html_tag {
    HTML_TAG_UNKNOWN = 0,
    HTML_TAG_HTML, HTML_TAG_HEAD, HTML_TAG_BODY, HTML_TAG_TITLE,
    HTML_TAG_META, HTML_TAG_LINK, HTML_TAG_STYLE, HTML_TAG_SCRIPT,
    HTML_TAG_BASE, HTML_TAG_NOSCRIPT, HTML_TAG_TEMPLATE,
    HTML_TAG_P, HTML_TAG_DIV, HTML_TAG_SPAN, HTML_TAG_A, HTML_TAG_IMG,
    HTML_TAG_BR, HTML_TAG_HR, HTML_TAG_WBR,
    HTML_TAG_H1, HTML_TAG_H2, HTML_TAG_H3, HTML_TAG_H4, HTML_TAG_H5,
    HTML_TAG_H6,
    HTML_TAG_UL, HTML_TAG_OL, HTML_TAG_LI, HTML_TAG_DL, HTML_TAG_DT,
    HTML_TAG_DD, HTML_TAG_MENU, HTML_TAG_DIR,
    HTML_TAG_PRE, HTML_TAG_LISTING, HTML_TAG_XMP, HTML_TAG_BLOCKQUOTE,
    HTML_TAG_TABLE, HTML_TAG_CAPTION, HTML_TAG_COLGROUP, HTML_TAG_COL,
    HTML_TAG_THEAD, HTML_TAG_TBODY, HTML_TAG_TFOOT, HTML_TAG_TR,
    HTML_TAG_TD, HTML_TAG_TH,
    HTML_TAG_B, HTML_TAG_STRONG, HTML_TAG_I, HTML_TAG_EM, HTML_TAG_U,
    HTML_TAG_S, HTML_TAG_STRIKE, HTML_TAG_DEL, HTML_TAG_INS, HTML_TAG_CODE,
    HTML_TAG_TT, HTML_TAG_KBD, HTML_TAG_SAMP, HTML_TAG_VAR, HTML_TAG_SMALL,
    HTML_TAG_BIG, HTML_TAG_SUB, HTML_TAG_SUP, HTML_TAG_FONT, HTML_TAG_NOBR,
    HTML_TAG_CITE, HTML_TAG_Q, HTML_TAG_ABBR, HTML_TAG_MARK, HTML_TAG_TIME,
    HTML_TAG_DFN, HTML_TAG_BDO, HTML_TAG_BDI, HTML_TAG_LABEL,
    HTML_TAG_FORM, HTML_TAG_INPUT, HTML_TAG_BUTTON, HTML_TAG_SELECT,
    HTML_TAG_OPTION, HTML_TAG_OPTGROUP, HTML_TAG_TEXTAREA,
    HTML_TAG_FIELDSET, HTML_TAG_LEGEND,
    HTML_TAG_NAV, HTML_TAG_HEADER, HTML_TAG_FOOTER, HTML_TAG_SECTION,
    HTML_TAG_ARTICLE, HTML_TAG_ASIDE, HTML_TAG_MAIN, HTML_TAG_FIGURE,
    HTML_TAG_FIGCAPTION, HTML_TAG_ADDRESS, HTML_TAG_DETAILS,
    HTML_TAG_SUMMARY, HTML_TAG_HGROUP, HTML_TAG_DIALOG, HTML_TAG_CENTER,
    HTML_TAG_IFRAME, HTML_TAG_OBJECT, HTML_TAG_EMBED, HTML_TAG_PARAM,
    HTML_TAG_AREA, HTML_TAG_MAP, HTML_TAG_SOURCE, HTML_TAG_TRACK,
    HTML_TAG_CANVAS, HTML_TAG_VIDEO, HTML_TAG_AUDIO, HTML_TAG_SVG,
    HTML_TAG_NOFRAMES, HTML_TAG_NOEMBED, HTML_TAG_FRAME, HTML_TAG_FRAMESET,
    HTML_TAG_COUNT
};

enum opos_html_node_type {
    HTML_NODE_ELEMENT = 1,
    HTML_NODE_TEXT = 2
};

struct opos_html_attr {
    const char *name;  /* lower case */
    const char *value; /* decoded; "" for a bare attribute */
    struct opos_html_attr *next;
};

struct opos_html_node {
    enum opos_html_node_type type;
    enum opos_html_tag tag;          /* elements: HTML_TAG_UNKNOWN if none */
    const char *name;                /* elements: lower-case tag name */
    const char *text;                /* text nodes: UTF-8, NUL-terminated */
    size_t text_length;
    struct opos_html_attr *attributes;
    struct opos_html_node *parent;
    struct opos_html_node *first_child;
    struct opos_html_node *last_child;
    struct opos_html_node *next_sibling;
    struct opos_html_node *previous_sibling;
};

struct opos_html_doc {
    struct opos_html_node *root;  /* the <html> element (always present) */
    struct opos_html_node *head;  /* always present */
    struct opos_html_node *body;  /* always present */
    const char *title;            /* text of the first <title>, trimmed */
    void *arena;                  /* private */
};

/* charset may be NULL. Returns NULL only when memory runs out. */
struct opos_html_doc *opos_html_parse(const char *data, size_t length,
                                      const char *charset);
void opos_html_free(struct opos_html_doc *doc);

/* The attribute's value or NULL. name must be lower case. */
const char *opos_html_attribute(const struct opos_html_node *node,
                                const char *name);

/* First element below node (including node itself) with this tag. */
struct opos_html_node *opos_html_find(struct opos_html_node *node,
                                      enum opos_html_tag tag);

/* Decodes character references in text (for attribute values the browser
   gets from elsewhere, such as CSS or URLs in scripts). out is NUL
   terminated; returns the length or the capacity-limited length. */
size_t opos_html_decode_entities(const char *text, size_t length, char *out,
                                 size_t capacity);

#endif
