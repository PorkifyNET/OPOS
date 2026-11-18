/*
 * Between script.c and canvas.c (the <canvas> natives).
 */
#ifndef BROWSER_CANVAS_H
#define BROWSER_CANVAS_H

#include <stdbool.h>

#include "quickjs.h"

struct page;
struct opos_html_node;

/* Adds N.cv to the natives object dom.js receives. */
void canvas_add_natives(JSContext *ctx, JSValue natives);

/* script.c: the node of a node object (NULL for anything else), the page
   the context runs for, whether two URLs have the same origin, and a
   fresh time limit after waiting for the network. */
struct opos_html_node *script_unwrap(JSValueConst value);
struct page *script_page(JSContext *ctx);
bool script_same_origin(const char *a, const char *b);
void script_more_time(JSContext *ctx);

#endif
