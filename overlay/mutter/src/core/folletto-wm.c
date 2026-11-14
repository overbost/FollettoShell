/* Folletto: i3-like tiling window manager living inside Mutter.
 *
 * In META_WM_MODE_NORMAL this module is inert. In META_WM_MODE_I3 it owns
 * geometry of tiled windows (layout-owned) and implements i3 commands.
 */

#include "config.h"

#include "core/folletto-wm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <graphene.h>

#include "core/display-private.h"
#include "core/window-private.h"
#include "core/workspace-private.h"
#include "meta/keybindings.h"
#include "meta/meta-backend.h"
#include "meta/meta-cursor-tracker.h"
#include "meta/meta-workspace-manager.h"
#include "meta/prefs.h"
#include "meta/util.h"
#include "meta/workspace.h"
#include "mtk/mtk.h"

#ifdef HAVE_X11_CLIENT
#include "x11/meta-x11-frame.h"
#endif

#define FOLLETTO_TAB_H 32

typedef enum
{
  FOLLETTO_LAYOUT_GRID,     /* auto: split binari H/V alternati sul focus */
  FOLLETTO_LAYOUT_NIRI,     /* strip orizzontale scrollabile (stile Niri) */
  FOLLETTO_LAYOUT_SPLITH,
  FOLLETTO_LAYOUT_SPLITV,
  FOLLETTO_LAYOUT_TABBED,
  FOLLETTO_LAYOUT_STACKED
} FollettoLayout;

typedef struct FollettoNode FollettoNode;

struct FollettoNode
{
  FollettoNode *parent;
  GPtrArray *children;
  MetaWindow *window;
  FollettoLayout layout;
  double percent;
  gboolean floating;
  MtkRectangle cell;
};

typedef struct
{
  int workspace;
  int monitor;
  FollettoNode *root;
  FollettoNode *focus;
  double view_offset; /* scroll orizzontale Niri, in pixel */
} FollettoTree;

struct _FollettoWm
{
  MetaDisplay *display;
  MetaWmMode mode;
  GHashTable *trees;
  GHashTable *nodes;
  GPtrArray *scratchpad;
  FollettoLayout next_layout;
  gboolean resize_mode;
  int gaps_inner;
  int gaps_outer;
  GSettings *settings;
  GPtrArray *grabbed_bindings;
  gboolean show_decorations;
  guint dbus_owner_id;
  guint dbus_reg_id;
  GDBusConnection *dbus_conn;
  GDBusNodeInfo *dbus_info;
  guint grab_idle_id;
  gpointer grab_job;
  MetaWindow *drag_window;
  gboolean layout_busy;
  guint layout_emit_id;
  guint layout_idle_id;
};

static void folletto_layout_tree (FollettoWm *wm, FollettoTree *tree);
static void folletto_insert_window (FollettoWm *wm, MetaWindow *window);
static void folletto_remove_window (FollettoWm *wm, MetaWindow *window);
static void queue_layout (FollettoWm *wm);
static void relayout_all (FollettoWm *wm);
static void folletto_grab_i3_keys (FollettoWm *wm);
static void folletto_ungrab_i3_keys (FollettoWm *wm);
static void emit_layout_changed (FollettoWm *wm);
static void on_show_decorations_changed (GSettings *settings,
                                         gchar     *key,
                                         FollettoWm *wm);
static void on_settings_changed (GSettings  *settings,
                                 gchar      *key,
                                 FollettoWm *wm);
static GVariant * folletto_wm_get_chrome (FollettoWm *wm);
static void folletto_wm_export_dbus (FollettoWm *wm);
static gboolean folletto_wm_export_dbus_idle (gpointer data);
static FollettoLayout layout_from_name (const char *name);
static gboolean tree_is_niri (FollettoWm *wm, FollettoTree *tree);
static void rebuild_as_niri (FollettoTree *tree);
static void rebuild_as_grid (FollettoTree *tree);
static void rebuild_as_tabs (FollettoTree *tree,
                             FollettoLayout layout);
static void insert_binary_split (FollettoTree   *tree,
                                 FollettoNode   *focus,
                                 FollettoNode   *leaf,
                                 FollettoLayout  layout);
static FollettoLayout grid_split_for_focus (FollettoNode *focus);
static void niri_layout_tree (FollettoWm *wm, FollettoTree *tree, const MtkRectangle *area);
static FollettoNode * niri_column_node (FollettoNode *leaf);
static void cmd_scroll_delta (FollettoWm *wm, double dx);
static void on_focus_window_niri (MetaDisplay *display,
                                  GParamSpec  *pspec,
                                  FollettoWm  *wm);

static guint
tree_key (int workspace,
          int monitor)
{
  return ((guint) workspace << 8) | ((guint) monitor & 0xff);
}

static FollettoNode *
node_new_leaf (MetaWindow *window)
{
  FollettoNode *n = g_new0 (FollettoNode, 1);
  n->window = window;
  n->percent = 1.0;
  n->layout = FOLLETTO_LAYOUT_SPLITH;
  return n;
}

static FollettoNode *
node_new_split (FollettoLayout layout)
{
  FollettoNode *n = g_new0 (FollettoNode, 1);
  n->children = g_ptr_array_new ();
  n->layout = layout;
  n->percent = 1.0;
  return n;
}

static gboolean
node_is_leaf (FollettoNode *n)
{
  return n->window != NULL;
}

static guint
count_leaves (FollettoNode *n)
{
  guint i, c = 0;

  if (!n)
    return 0;
  if (node_is_leaf (n))
    return 1;
  if (!n->children)
    return 0;
  for (i = 0; i < n->children->len; i++)
    c += count_leaves (g_ptr_array_index (n->children, i));
  return c;
}

static gboolean
node_is_tab_container (FollettoNode *n)
{
  return n &&
         (n->layout == FOLLETTO_LAYOUT_TABBED ||
          n->layout == FOLLETTO_LAYOUT_STACKED);
}

static void
node_free (FollettoNode *n)
{
  guint i;

  if (!n)
    return;

  if (n->children)
    {
      for (i = 0; i < n->children->len; i++)
        node_free (g_ptr_array_index (n->children, i));
      g_ptr_array_free (n->children, TRUE);
    }
  g_free (n);
}

static FollettoNode *
first_leaf (FollettoNode *n)
{
  if (!n)
    return NULL;
  if (node_is_leaf (n))
    return n;
  if (n->children && n->children->len > 0)
    return first_leaf (g_ptr_array_index (n->children, 0));
  return NULL;
}

static FollettoTree *
get_tree (FollettoWm *wm,
          int         workspace,
          int         monitor)
{
  guint key = tree_key (workspace, monitor);
  FollettoTree *tree = g_hash_table_lookup (wm->trees, GUINT_TO_POINTER (key));

  if (!tree)
    {
      tree = g_new0 (FollettoTree, 1);
      tree->workspace = workspace;
      tree->monitor = monitor;
      g_hash_table_insert (wm->trees, GUINT_TO_POINTER (key), tree);
    }
  return tree;
}

static FollettoTree *
tree_for_window (FollettoWm *wm,
                 MetaWindow *window)
{
  MetaWorkspace *ws;
  int workspace = 0;
  int monitor = 0;

  ws = meta_window_get_workspace (window);
  if (ws)
    workspace = meta_workspace_index (ws);
  monitor = meta_window_get_monitor (window);
  if (monitor < 0)
    monitor = 0;
  return get_tree (wm, workspace, monitor);
}

static gboolean
should_manage (MetaWindow *window)
{
  MetaWindowType type;

  if (!window || window->override_redirect)
    return FALSE;

  type = meta_window_get_window_type (window);
  switch (type)
    {
    case META_WINDOW_NORMAL:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
should_float (MetaWindow *window)
{
  MetaWindowType type = meta_window_get_window_type (window);

  switch (type)
    {
    case META_WINDOW_DIALOG:
    case META_WINDOW_MODAL_DIALOG:
    case META_WINDOW_SPLASHSCREEN:
    case META_WINDOW_UTILITY:
    case META_WINDOW_TOOLBAR:
      return TRUE;
    default:
      return window->transient_for != NULL;
    }
}

static void
apply_chrome (FollettoWm *wm,
              MetaWindow *window,
              gboolean    tiled)
{
  gboolean hide;
  gboolean changed;

  if (!window)
    return;

  hide = tiled && !wm->show_decorations;
  changed = window->hide_decorations != hide;

  window->hide_decorations = hide;
  /* Niri posiziona colonne anche fuori dallo schermo: niente vincoli ai bordi. */
  if (tiled && wm->next_layout == FOLLETTO_LAYOUT_NIRI)
    {
      window->edge_constraints.top = META_EDGE_CONSTRAINT_NONE;
      window->edge_constraints.right = META_EDGE_CONSTRAINT_NONE;
      window->edge_constraints.bottom = META_EDGE_CONSTRAINT_NONE;
      window->edge_constraints.left = META_EDGE_CONSTRAINT_NONE;
    }
  else if (hide)
    {
      window->edge_constraints.top = META_EDGE_CONSTRAINT_MONITOR;
      window->edge_constraints.right = META_EDGE_CONSTRAINT_MONITOR;
      window->edge_constraints.bottom = META_EDGE_CONSTRAINT_MONITOR;
      window->edge_constraints.left = META_EDGE_CONSTRAINT_MONITOR;
    }
  else
    {
      window->edge_constraints.top = META_EDGE_CONSTRAINT_NONE;
      window->edge_constraints.right = META_EDGE_CONSTRAINT_NONE;
      window->edge_constraints.bottom = META_EDGE_CONSTRAINT_NONE;
      window->edge_constraints.left = META_EDGE_CONSTRAINT_NONE;
    }
  if (changed)
    meta_window_recalc_features (window);
}

static void
apply_rect (MetaWindow         *window,
            const MtkRectangle *rect)
{
  FollettoWm *wm;

  if (!window || window->unmanaging)
    return;

  wm = window->display->folletto_wm;
  if (wm && wm->drag_window == window)
    return;
  window->layout_owned = TRUE;
  if (wm)
    apply_chrome (wm, window, TRUE);
  meta_window_move_resize_frame (window, FALSE,
                                 rect->x, rect->y,
                                 MAX (rect->width, 1),
                                 MAX (rect->height, 1));
}

static void
content_area_for_tabs (FollettoNode       *node,
                       const MtkRectangle *area,
                       MtkRectangle       *content)
{
  int ntabs;
  int bar;

  *content = *area;
  if (!node_is_tab_container (node))
    return;

  ntabs = MAX (count_leaves (node), 1);
  bar = FOLLETTO_TAB_H * (node->layout == FOLLETTO_LAYOUT_STACKED ? ntabs : 1);
  if (bar >= area->height)
    bar = MAX (area->height / 4, 1);
  content->y += bar;
  content->height = MAX (area->height - bar, 1);
}

static void
layout_node (FollettoWm          *wm,
             FollettoNode        *node,
             const MtkRectangle  *area,
             FollettoNode        *focus)
{
  MtkRectangle child_area;
  MtkRectangle content;
  guint i;
  int gap = wm->gaps_inner;
  double total = 0;
  int cursor;

  if (!node)
    return;

  node->cell = *area;

  if (node_is_leaf (node))
    {
      if (node->window && !node->floating && !node->window->minimized)
        {
          /* Only the tab container reserves the title strip. A leaf inside a
           * tabbed/stacked parent already received the shrunk content area. */
          if (node_is_tab_container (node) && !node->parent)
            content_area_for_tabs (node, area, &content);
          else
            content = *area;
          apply_rect (node->window, &content);
        }
      return;
    }

  if (!node->children || node->children->len == 0)
    return;

  if (node_is_tab_container (node))
    {
      FollettoNode *active = NULL;

      content_area_for_tabs (node, area, &content);
      /* Tutte le tab condividono l’area contenuto; quella attiva viene alzata.
       * (Niente off-screen: altrimenti le nuove finestre restavano invisibili.) */
      if (focus)
        {
          FollettoNode *walk = focus;

          while (walk)
            {
              if (walk->parent == node)
                {
                  active = walk;
                  break;
                }
              walk = walk->parent;
            }
        }
      if (!active && node->children->len > 0)
        active = g_ptr_array_index (node->children, 0);

      for (i = 0; i < node->children->len; i++)
        {
          FollettoNode *ch = g_ptr_array_index (node->children, i);

          layout_node (wm, ch, &content, focus);
        }
      if (active && active->window && !active->window->unmanaging)
        meta_window_raise (active->window);
      return;
    }

  for (i = 0; i < node->children->len; i++)
    {
      FollettoNode *ch = g_ptr_array_index (node->children, i);
      total += MAX (ch->percent, 0.01);
    }

  cursor = (node->layout == FOLLETTO_LAYOUT_SPLITH) ? area->x : area->y;

  for (i = 0; i < node->children->len; i++)
    {
      FollettoNode *ch = g_ptr_array_index (node->children, i);
      double share = MAX (ch->percent, 0.01) / total;
      int size;
      int avail;

      child_area = *area;
      if (node->layout == FOLLETTO_LAYOUT_SPLITH)
        {
          avail = area->width - gap * (int) (node->children->len - 1);
          size = (int) round (avail * share);
          if (i == node->children->len - 1)
            size = area->x + area->width - cursor;
          child_area.x = cursor;
          child_area.width = MAX (size, 1);
          cursor += size + gap;
        }
      else
        {
          avail = area->height - gap * (int) (node->children->len - 1);
          size = (int) round (avail * share);
          if (i == node->children->len - 1)
            size = area->y + area->height - cursor;
          child_area.y = cursor;
          child_area.height = MAX (size, 1);
          cursor += size + gap;
        }
      layout_node (wm, ch, &child_area, focus);
    }
}

static void
sync_tree_focus (FollettoWm   *wm,
                 FollettoTree *tree)
{
  MetaWindow *fw;
  FollettoNode *n;

  if (!wm || !tree)
    return;
  fw = meta_display_get_focus_window (wm->display);
  if (!fw)
    return;
  n = g_hash_table_lookup (wm->nodes, fw);
  if (n && !n->floating)
    tree->focus = n;
}

static void
folletto_layout_tree (FollettoWm   *wm,
                      FollettoTree *tree)
{
  MetaWorkspaceManager *wsm;
  MetaWorkspace *ws;
  MtkRectangle area;

  if (!tree || !tree->root)
    return;

  wsm = meta_display_get_workspace_manager (wm->display);
  ws = meta_workspace_manager_get_workspace_by_index (wsm, tree->workspace);
  if (!ws)
    return;

  meta_workspace_get_work_area_for_monitor (ws, tree->monitor, &area);
  area.x += wm->gaps_outer;
  area.y += wm->gaps_outer;
  area.width = MAX (area.width - 2 * wm->gaps_outer, 1);
  area.height = MAX (area.height - 2 * wm->gaps_outer, 1);
  sync_tree_focus (wm, tree);
  wm->layout_busy = TRUE;
  if (tree_is_niri (wm, tree))
    niri_layout_tree (wm, tree, &area);
  else
    layout_node (wm, tree->root, &area, tree->focus);
  wm->layout_busy = FALSE;
}

static void
normalize_percents (FollettoNode *parent)
{
  guint i;
  double t = 0;

  if (!parent->children || parent->children->len == 0)
    return;
  for (i = 0; i < parent->children->len; i++)
    {
      FollettoNode *ch = g_ptr_array_index (parent->children, i);
      t += MAX (ch->percent, 0.01);
    }
  for (i = 0; i < parent->children->len; i++)
    {
      FollettoNode *ch = g_ptr_array_index (parent->children, i);
      ch->percent = MAX (ch->percent, 0.01) / t;
    }
}

static void
replace_in_parent (FollettoNode *old,
                   FollettoNode *nw)
{
  FollettoNode *p = old->parent;
  guint i;

  nw->parent = p;
  nw->percent = old->percent;
  if (!p)
    return;
  for (i = 0; i < p->children->len; i++)
    {
      if (g_ptr_array_index (p->children, i) == old)
        {
          g_ptr_array_index (p->children, i) = nw;
          return;
        }
    }
}

static FollettoNode *
focused_leaf (FollettoWm *wm)
{
  MetaWindow *fw = meta_display_get_focus_window (wm->display);
  FollettoNode *n;

  if (fw)
    {
      n = g_hash_table_lookup (wm->nodes, fw);
      if (n)
        return n;
    }
  return NULL;
}

static FollettoTree *
focused_tree (FollettoWm *wm)
{
  FollettoNode *leaf = focused_leaf (wm);
  MetaWorkspaceManager *wsm;
  int ws, mon;

  if (leaf && leaf->window)
    return tree_for_window (wm, leaf->window);

  wsm = meta_display_get_workspace_manager (wm->display);
  ws = meta_workspace_manager_get_active_workspace_index (wsm);
  mon = meta_display_get_current_monitor (wm->display);
  return get_tree (wm, ws, mon);
}

static const char *
layout_name (FollettoLayout layout)
{
  switch (layout)
    {
    case FOLLETTO_LAYOUT_GRID:
      return "grid";
    case FOLLETTO_LAYOUT_NIRI:
      return "niri";
    case FOLLETTO_LAYOUT_SPLITV:
      return "splitv";
    case FOLLETTO_LAYOUT_TABBED:
      return "tabbed";
    case FOLLETTO_LAYOUT_STACKED:
      return "stacked";
    case FOLLETTO_LAYOUT_SPLITH:
    default:
      return "splith";
    }
}

const char *
folletto_wm_get_layout (FollettoWm *wm)
{
  FollettoNode *leaf;

  if (!wm || wm->mode != META_WM_MODE_I3)
    return "normal";

  leaf = focused_leaf (wm);
  if (leaf && leaf->floating)
    return "floating";
  /* Modalità workspace: next_layout vince su eventuali split locali. */
  if (wm->next_layout == FOLLETTO_LAYOUT_GRID)
    return "grid";
  if (wm->next_layout == FOLLETTO_LAYOUT_NIRI)
    return "niri";
  if (wm->next_layout == FOLLETTO_LAYOUT_TABBED)
    return "tabbed";
  if (wm->next_layout == FOLLETTO_LAYOUT_STACKED)
    return "stacked";
  if (!leaf)
    return layout_name (wm->next_layout);
  if (leaf->parent)
    return layout_name (leaf->parent->layout);
  return layout_name (leaf->layout);
}

/* Prossimo asse in modalità griglia: prima metà SX/DX, poi alterna. */
static FollettoLayout
grid_split_for_focus (FollettoNode *focus)
{
  if (focus && focus->parent &&
      (focus->parent->layout == FOLLETTO_LAYOUT_SPLITH ||
       focus->parent->layout == FOLLETTO_LAYOUT_SPLITV))
    {
      return focus->parent->layout == FOLLETTO_LAYOUT_SPLITH
             ? FOLLETTO_LAYOUT_SPLITV
             : FOLLETTO_LAYOUT_SPLITH;
    }
  return FOLLETTO_LAYOUT_SPLITH;
}

static FollettoLayout
geometry_layout (FollettoLayout layout)
{
  if (layout == FOLLETTO_LAYOUT_GRID || layout == FOLLETTO_LAYOUT_NIRI)
    return FOLLETTO_LAYOUT_SPLITH;
  return layout;
}

static gboolean
tree_is_niri (FollettoWm  *wm,
              FollettoTree *tree)
{
  if (!wm || wm->next_layout != FOLLETTO_LAYOUT_NIRI)
    return FALSE;
  return tree && tree->root != NULL;
}

static FollettoNode *
niri_column_node (FollettoNode *leaf)
{
  if (!leaf)
    return NULL;
  if (!leaf->parent)
    return leaf;
  if (leaf->parent->layout == FOLLETTO_LAYOUT_NIRI)
    return leaf;
  if ((leaf->parent->layout == FOLLETTO_LAYOUT_SPLITV ||
       leaf->parent->layout == FOLLETTO_LAYOUT_SPLITH ||
       leaf->parent->layout == FOLLETTO_LAYOUT_TABBED ||
       leaf->parent->layout == FOLLETTO_LAYOUT_STACKED) &&
      leaf->parent->parent &&
      leaf->parent->parent->layout == FOLLETTO_LAYOUT_NIRI)
    return leaf->parent;
  return leaf;
}

static double
niri_column_width_px (FollettoNode *col,
                      int           area_width)
{
  double frac = (col && col->percent > 0.01) ? col->percent : 0.5;

  return MAX (frac * (double) area_width, 64.0);
}

static void
free_containers_keep_leaves (FollettoNode *n,
                             GPtrArray    *leaves)
{
  guint i;

  if (!n)
    return;
  if (node_is_leaf (n))
    {
      n->parent = NULL;
      g_ptr_array_add (leaves, n);
      return;
    }
  if (n->children)
    {
      for (i = 0; i < n->children->len; i++)
        free_containers_keep_leaves (g_ptr_array_index (n->children, i), leaves);
      g_ptr_array_free (n->children, TRUE);
      n->children = NULL;
    }
  g_free (n);
}

static void
rebuild_as_niri (FollettoTree *tree)
{
  GPtrArray *leaves;
  FollettoNode *strip;
  FollettoNode *old_focus;
  guint i;

  if (!tree)
    return;
  if (tree->root && tree->root->layout == FOLLETTO_LAYOUT_NIRI)
    return;

  leaves = g_ptr_array_new ();
  old_focus = tree->focus;
  if (tree->root)
    {
      if (node_is_leaf (tree->root))
        {
          tree->root->parent = NULL;
          g_ptr_array_add (leaves, tree->root);
        }
      else
        {
          free_containers_keep_leaves (tree->root, leaves);
        }
    }
  tree->root = NULL;

  strip = node_new_split (FOLLETTO_LAYOUT_NIRI);
  for (i = 0; i < leaves->len; i++)
    {
      FollettoNode *L = g_ptr_array_index (leaves, i);

      if (L->floating)
        continue;
      L->parent = strip;
      L->percent = 0.5;
      g_ptr_array_add (strip->children, L);
    }
  tree->root = strip;
  tree->view_offset = 0;
  if (old_focus && !old_focus->floating && old_focus->parent == strip)
    tree->focus = old_focus;
  else
    tree->focus = first_leaf (strip);
  g_ptr_array_free (leaves, TRUE);
}

/* Ricostruisce l’albero come griglia binaria H/V (stesso algoritmo di inserimento). */
static void
rebuild_as_grid (FollettoTree *tree)
{
  GPtrArray *leaves;
  GPtrArray *tiled;
  FollettoNode *old_focus;
  guint i;

  if (!tree)
    return;

  leaves = g_ptr_array_new ();
  tiled = g_ptr_array_new ();
  old_focus = tree->focus;
  if (tree->root)
    {
      if (node_is_leaf (tree->root))
        {
          tree->root->parent = NULL;
          g_ptr_array_add (leaves, tree->root);
        }
      else
        {
          free_containers_keep_leaves (tree->root, leaves);
        }
    }
  tree->root = NULL;
  tree->view_offset = 0;

  for (i = 0; i < leaves->len; i++)
    {
      FollettoNode *L = g_ptr_array_index (leaves, i);

      if (L->floating)
        continue;
      L->parent = NULL;
      L->percent = 1.0;
      g_ptr_array_add (tiled, L);
    }

  if (tiled->len == 0)
    {
      tree->focus = NULL;
    }
  else if (tiled->len == 1)
    {
      FollettoNode *L = g_ptr_array_index (tiled, 0);

      tree->root = L;
      tree->focus = L;
    }
  else
    {
      FollettoNode *first = g_ptr_array_index (tiled, 0);

      tree->root = first;
      tree->focus = first;
      for (i = 1; i < tiled->len; i++)
        {
          FollettoNode *L = g_ptr_array_index (tiled, i);
          FollettoNode *target = tree->focus;

          insert_binary_split (tree, target, L, grid_split_for_focus (target));
        }
    }

  if (old_focus && !old_focus->floating &&
      (old_focus->parent != NULL || tree->root == old_focus))
    tree->focus = old_focus;
  else if (tree->root)
    tree->focus = first_leaf (tree->root);

  g_ptr_array_free (leaves, TRUE);
  g_ptr_array_free (tiled, TRUE);
}

/* Un unico container tabbed/stacked con tutte le foglie tiled del workspace. */
static void
rebuild_as_tabs (FollettoTree  *tree,
                 FollettoLayout layout)
{
  GPtrArray *leaves;
  GPtrArray *tiled;
  FollettoNode *old_focus;
  FollettoNode *tabs;
  guint i;

  if (!tree)
    return;
  if (layout != FOLLETTO_LAYOUT_TABBED && layout != FOLLETTO_LAYOUT_STACKED)
    return;

  leaves = g_ptr_array_new ();
  tiled = g_ptr_array_new ();
  old_focus = tree->focus;
  if (tree->root)
    {
      if (node_is_leaf (tree->root))
        {
          tree->root->parent = NULL;
          g_ptr_array_add (leaves, tree->root);
        }
      else
        {
          free_containers_keep_leaves (tree->root, leaves);
        }
    }
  tree->root = NULL;
  tree->view_offset = 0;

  for (i = 0; i < leaves->len; i++)
    {
      FollettoNode *L = g_ptr_array_index (leaves, i);

      if (L->floating)
        continue;
      L->parent = NULL;
      L->percent = 1.0;
      g_ptr_array_add (tiled, L);
    }

  if (tiled->len == 0)
    {
      tree->focus = NULL;
      g_ptr_array_free (leaves, TRUE);
      g_ptr_array_free (tiled, TRUE);
      return;
    }

  if (tiled->len == 1)
    {
      FollettoNode *L = g_ptr_array_index (tiled, 0);

      /* Anche una sola finestra: container tabbed così compare la barra. */
      tabs = node_new_split (layout);
      L->parent = tabs;
      L->percent = 1.0;
      g_ptr_array_add (tabs->children, L);
      tree->root = tabs;
      tree->focus = L;
    }
  else
    {
      tabs = node_new_split (layout);
      for (i = 0; i < tiled->len; i++)
        {
          FollettoNode *L = g_ptr_array_index (tiled, i);

          L->parent = tabs;
          L->percent = 1.0 / tiled->len;
          g_ptr_array_add (tabs->children, L);
        }
      tree->root = tabs;
      if (old_focus && !old_focus->floating && old_focus->parent == tabs)
        tree->focus = old_focus;
      else
        tree->focus = g_ptr_array_index (tiled, 0);
    }

  g_ptr_array_free (leaves, TRUE);
  g_ptr_array_free (tiled, TRUE);
}

static void
niri_ensure_visible (FollettoTree *tree,
                     int           area_width,
                     int           gap)
{
  FollettoNode *col;
  FollettoNode *strip;
  double x = 0;
  double w;
  double max_off = 0;
  double total = 0;
  guint i;

  if (!tree || !tree->root || !tree->root->children || !tree->focus)
    return;

  strip = tree->root;
  col = niri_column_node (tree->focus);

  for (i = 0; i < strip->children->len; i++)
    {
      FollettoNode *c = g_ptr_array_index (strip->children, i);

      w = niri_column_width_px (c, area_width);
      total += w;
      if (i + 1 < strip->children->len)
        total += gap;
    }
  max_off = MAX (total - area_width, 0);

  for (i = 0; i < strip->children->len; i++)
    {
      FollettoNode *c = g_ptr_array_index (strip->children, i);

      w = niri_column_width_px (c, area_width);
      if (c == col)
        {
          if (x < tree->view_offset)
            tree->view_offset = x;
          if (x + w > tree->view_offset + area_width)
            tree->view_offset = x + w - area_width;
          break;
        }
      x += w + gap;
    }

  tree->view_offset = CLAMP (tree->view_offset, 0, max_off);
}

static void
niri_layout_tree (FollettoWm          *wm,
                  FollettoTree        *tree,
                  const MtkRectangle  *area)
{
  FollettoNode *strip = tree->root;
  double cursor;
  double total = 0;
  double max_off;
  guint i;
  int gap = wm->gaps_inner;

  if (!strip)
    return;

  if (node_is_leaf (strip))
    {
      MtkRectangle r = *area;
      double frac = strip->percent > 0.01 ? strip->percent : 0.5;

      r.width = MAX ((int) round (area->width * frac), 1);
      strip->cell = r;
      if (strip->window && !strip->floating && !strip->window->minimized)
        apply_rect (strip->window, &r);
      return;
    }

  if (!strip->children)
    return;

  niri_ensure_visible (tree, area->width, gap);

  for (i = 0; i < strip->children->len; i++)
    {
      FollettoNode *c = g_ptr_array_index (strip->children, i);

      total += niri_column_width_px (c, area->width);
      if (i + 1 < strip->children->len)
        total += gap;
    }
  max_off = MAX (total - area->width, 0);
  tree->view_offset = CLAMP (tree->view_offset, 0, max_off);

  cursor = area->x - tree->view_offset;
  for (i = 0; i < strip->children->len; i++)
    {
      FollettoNode *c = g_ptr_array_index (strip->children, i);
      MtkRectangle col_area;
      int w = (int) round (niri_column_width_px (c, area->width));

      col_area.x = (int) round (cursor);
      col_area.y = area->y;
      col_area.width = MAX (w, 1);
      col_area.height = area->height;
      c->cell = col_area;

      if (node_is_leaf (c))
        {
          if (c->window && !c->floating && !c->window->minimized)
            apply_rect (c->window, &col_area);
        }
      else
        {
          layout_node (wm, c, &col_area, tree->focus);
        }
      cursor += w + gap;
    }
}

static void
emit_layout_changed (FollettoWm *wm)
{
  if (!wm || !wm->display || wm->layout_busy || wm->drag_window)
    return;
  g_signal_emit_by_name (wm->display, "i3-layout-changed",
                         folletto_wm_get_layout (wm));
}

static void
foreach_leaf (FollettoNode *n,
              void        (*fn) (FollettoNode *, gpointer),
              gpointer      data)
{
  guint i;

  if (!n)
    return;
  if (node_is_leaf (n))
    {
      fn (n, data);
      return;
    }
  if (!n->children)
    return;
  for (i = 0; i < n->children->len; i++)
    foreach_leaf (g_ptr_array_index (n->children, i), fn, data);
}

static void
set_leaf_floating (FollettoNode *n,
                   gpointer      data)
{
  gboolean floating = GPOINTER_TO_INT (data);
  FollettoWm *wm;

  n->floating = floating;
  if (!n->window)
    return;
  n->window->layout_owned = !floating;
  wm = n->window->display->folletto_wm;
  if (wm)
    apply_chrome (wm, n->window, !floating);
}

static void
unfloat_tree (FollettoTree *tree)
{
  if (tree && tree->root)
    foreach_leaf (tree->root, set_leaf_floating, GINT_TO_POINTER (FALSE));
}

static int
node_axis_size (FollettoNode *n,
                gboolean      horizontal)
{
  MtkRectangle r;
  guint i;
  int size = 0;

  if (!n)
    return 1;
  if (node_is_leaf (n) && n->window)
    {
      meta_window_get_frame_rect (n->window, &r);
      return MAX (horizontal ? r.width : r.height, 1);
    }
  if (!n->children)
    return 1;
  for (i = 0; i < n->children->len; i++)
    size += node_axis_size (g_ptr_array_index (n->children, i), horizontal);
  return MAX (size, 1);
}

static void
sync_split_from_geometry (FollettoNode *leaf)
{
  FollettoNode *parent;
  gboolean horizontal;
  guint i;

  if (!leaf || !leaf->parent)
    return;
  parent = leaf->parent;
  if (parent->layout == FOLLETTO_LAYOUT_TABBED ||
      parent->layout == FOLLETTO_LAYOUT_STACKED)
    return;
  horizontal = (parent->layout == FOLLETTO_LAYOUT_SPLITH);
  for (i = 0; i < parent->children->len; i++)
    {
      FollettoNode *ch = g_ptr_array_index (parent->children, i);
      ch->percent = node_axis_size (ch, horizontal);
    }
  normalize_percents (parent);
}

static void
swap_leaves (FollettoNode *a,
             FollettoNode *b)
{
  FollettoNode *pa, *pb;
  guint i, ia = 0, ib = 0;
  double tmp;

  if (!a || !b || a == b)
    return;
  pa = a->parent;
  pb = b->parent;
  if (!pa || !pb || !pa->children || !pb->children)
    return;
  {
    gboolean found_a = FALSE, found_b = FALSE;

    for (i = 0; i < pa->children->len; i++)
      {
        if (g_ptr_array_index (pa->children, i) == a)
          {
            ia = i;
            found_a = TRUE;
          }
      }
    for (i = 0; i < pb->children->len; i++)
      {
        if (g_ptr_array_index (pb->children, i) == b)
          {
            ib = i;
            found_b = TRUE;
          }
      }
    if (!found_a || !found_b)
      return;
  }
  g_ptr_array_index (pa->children, ia) = b;
  g_ptr_array_index (pb->children, ib) = a;
  a->parent = pb;
  b->parent = pa;
  tmp = a->percent;
  a->percent = b->percent;
  b->percent = tmp;
}

typedef struct
{
  FollettoNode *except;
  FollettoNode *hit;
  FollettoNode *nearest;
  double nearest_d;
  int x;
  int y;
} LeafPick;

static void
pick_leaf_at_point (FollettoNode *n,
                    gpointer      data)
{
  LeafPick *pick = data;
  double cx, cy, dx, dy, d;

  if (n == pick->except || n->floating || !n->window)
    return;
  if (mtk_rectangle_contains_point (&n->cell, pick->x, pick->y))
    {
      pick->hit = n;
      return;
    }
  cx = n->cell.x + n->cell.width / 2.0;
  cy = n->cell.y + n->cell.height / 2.0;
  dx = pick->x - cx;
  dy = pick->y - cy;
  d = dx * dx + dy * dy;
  if (d < pick->nearest_d)
    {
      pick->nearest_d = d;
      pick->nearest = n;
    }
}

static FollettoNode *
leaf_at_xy (FollettoWm   *wm,
            FollettoNode *except,
            int           x,
            int           y)
{
  FollettoTree *tree;
  LeafPick pick = {
    .except = except,
    .nearest_d = G_MAXDOUBLE,
    .x = x,
    .y = y,
  };

  tree = focused_tree (wm);
  if (!tree || !tree->root)
    return NULL;
  foreach_leaf (tree->root, pick_leaf_at_point, &pick);
  return pick.hit ? pick.hit : pick.nearest;
}

static gboolean
pointer_xy (FollettoWm *wm,
            int        *x,
            int        *y)
{
  MetaContext *context;
  MetaBackend *backend;
  MetaCursorTracker *tracker;
  graphene_point_t point;

  context = meta_display_get_context (wm->display);
  if (!context)
    return FALSE;
  backend = meta_context_get_backend (context);
  if (!backend)
    return FALSE;
  tracker = meta_backend_get_cursor_tracker (backend);
  if (!tracker)
    return FALSE;
  meta_cursor_tracker_get_pointer (tracker, &point, NULL);
  *x = (int) point.x;
  *y = (int) point.y;
  return TRUE;
}

typedef struct
{
  FollettoWm *wm;
  MetaWindow *window;
  MetaGrabOp op;
  int x;
  int y;
} FollettoGrabIdle;

static void
cancel_grab_job (FollettoWm *wm)
{
  FollettoGrabIdle *old;

  if (!wm->grab_idle_id)
    return;
  old = wm->grab_job;
  g_source_remove (wm->grab_idle_id);
  wm->grab_idle_id = 0;
  wm->grab_job = NULL;
  if (old)
    {
      if (old->window)
        g_object_remove_weak_pointer (G_OBJECT (old->window),
                                      (gpointer *) &old->window);
      g_free (old);
    }
}

static gboolean
on_grab_op_end_idle (gpointer data)
{
  FollettoGrabIdle *job = data;
  FollettoWm *wm = job->wm;
  MetaWindow *window = job->window;
  FollettoNode *node, *other_node;
  FollettoTree *tree;

  wm->grab_idle_id = 0;
  wm->grab_job = NULL;
  wm->drag_window = NULL;

  if (wm->mode != META_WM_MODE_I3 || !window || window->unmanaging)
    {
      if (window)
        g_object_remove_weak_pointer (G_OBJECT (window),
                                      (gpointer *) &job->window);
      g_free (job);
      return G_SOURCE_REMOVE;
    }
  g_object_remove_weak_pointer (G_OBJECT (window), (gpointer *) &job->window);

  node = g_hash_table_lookup (wm->nodes, window);
  if (!node || node->floating)
    {
      g_free (job);
      return G_SOURCE_REMOVE;
    }

  emit_layout_changed (wm);

  if (meta_grab_op_is_moving (job->op))
    {
      other_node = leaf_at_xy (wm, node, job->x, job->y);
      if (other_node && other_node != node && !other_node->floating &&
          other_node->parent && node->parent)
        swap_leaves (node, other_node);
    }
  else if (meta_grab_op_is_resizing (job->op))
    {
      sync_split_from_geometry (node);
    }

  tree = tree_for_window (wm, window);
  if (tree)
    folletto_layout_tree (wm, tree);

  g_free (job);
  return G_SOURCE_REMOVE;
}

static void
on_grab_op_begin (MetaDisplay *display,
                  MetaWindow  *window,
                  MetaGrabOp   op,
                  FollettoWm  *wm)
{
  (void) display;
  if (wm->mode != META_WM_MODE_I3 || !window || window->unmanaging)
    return;
  if (meta_grab_op_is_moving (op) || meta_grab_op_is_resizing (op))
    {
      wm->drag_window = window;
      window->layout_owned = FALSE;
    }
}

static void
on_grab_op_end (MetaDisplay *display,
                MetaWindow  *window,
                MetaGrabOp   op,
                FollettoWm  *wm)
{
  FollettoGrabIdle *job;
  int x = 0, y = 0;

  (void) display;
  cancel_grab_job (wm);

  if (wm->mode != META_WM_MODE_I3 || !window || window->unmanaging)
    {
      wm->drag_window = NULL;
      return;
    }
  if (!meta_grab_op_is_moving (op) && !meta_grab_op_is_resizing (op))
    {
      wm->drag_window = NULL;
      return;
    }

  pointer_xy (wm, &x, &y);
  job = g_new0 (FollettoGrabIdle, 1);
  job->wm = wm;
  job->window = window;
  job->op = op;
  job->x = x;
  job->y = y;
  g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &job->window);
  wm->grab_job = job;
  /* After the Clutter grab is fully dismissed: swap/snap, never during end. */
  wm->grab_idle_id = g_timeout_add (200, on_grab_op_end_idle, job);
}

static void
insert_as_sibling (FollettoTree *tree,
                   FollettoNode *focus,
                   FollettoNode *leaf)
{
  FollettoNode *parent = focus->parent;
  guint idx = 0, i;

  for (i = 0; i < parent->children->len; i++)
    {
      if (g_ptr_array_index (parent->children, i) == focus)
        {
          idx = i + 1;
          break;
        }
    }
  leaf->parent = parent;
  g_ptr_array_insert (parent->children, (int) idx, leaf);
  normalize_percents (parent);
  tree->focus = leaf;
}

/* Divide il nodo focus a metà e colloca la nuova foglia nell'altra metà. */
static void
insert_binary_split (FollettoTree   *tree,
                     FollettoNode   *focus,
                     FollettoNode   *leaf,
                     FollettoLayout  layout)
{
  FollettoNode *split = node_new_split (geometry_layout (layout));
  FollettoNode *target = focus;

  if (!target)
    target = tree->root;

  if (!target || !target->parent)
    {
      if (tree->root)
        {
          g_ptr_array_add (split->children, tree->root);
          tree->root->parent = split;
          tree->root->percent = 0.5;
        }
      g_ptr_array_add (split->children, leaf);
      leaf->parent = split;
      leaf->percent = 0.5;
      tree->root = split;
    }
  else
    {
      replace_in_parent (target, split);
      target->parent = split;
      target->percent = 0.5;
      g_ptr_array_add (split->children, target);
      leaf->parent = split;
      leaf->percent = 0.5;
      g_ptr_array_add (split->children, leaf);
    }
  tree->focus = leaf;
}

static void
folletto_insert_window (FollettoWm *wm,
                        MetaWindow *window)
{
  FollettoTree *tree;
  FollettoNode *leaf;
  FollettoNode *focus;

  if (wm->mode != META_WM_MODE_I3)
    return;
  if (!should_manage (window))
    return;
  if (g_hash_table_lookup (wm->nodes, window))
    return;

  tree = tree_for_window (wm, window);
  leaf = node_new_leaf (window);
  g_hash_table_insert (wm->nodes, window, leaf);

  if (should_float (window))
    {
      leaf->floating = TRUE;
      window->layout_owned = FALSE;
      apply_chrome (wm, window, FALSE);
      tree->focus = leaf;
      return;
    }

  focus = tree->focus;
  if (!tree->root)
    {
      if (wm->next_layout == FOLLETTO_LAYOUT_NIRI)
        {
          FollettoNode *strip = node_new_split (FOLLETTO_LAYOUT_NIRI);

          /* Default Niri: metà schermo, non full-bleed. */
          leaf->percent = 0.5;
          leaf->parent = strip;
          g_ptr_array_add (strip->children, leaf);
          tree->root = strip;
          tree->focus = leaf;
          tree->view_offset = 0;
        }
      else if (wm->next_layout == FOLLETTO_LAYOUT_TABBED ||
               wm->next_layout == FOLLETTO_LAYOUT_STACKED)
        {
          FollettoNode *tabs = node_new_split (wm->next_layout);

          leaf->parent = tabs;
          leaf->percent = 1.0;
          g_ptr_array_add (tabs->children, leaf);
          tree->root = tabs;
          tree->focus = leaf;
        }
      else
        {
          /* Prima finestra (griglia/i3): schermo intero. */
          tree->root = leaf;
          tree->focus = leaf;
        }
    }
  else if (wm->next_layout == FOLLETTO_LAYOUT_NIRI)
    {
      FollettoNode *strip;
      FollettoNode *target = (focus && !focus->floating) ? focus : NULL;
      FollettoNode *parent;
      FollettoNode *col;
      guint idx = 0, i;

      if (tree->root->layout != FOLLETTO_LAYOUT_NIRI)
        rebuild_as_niri (tree);
      strip = tree->root;
      leaf->percent = 0.5;
      parent = target ? target->parent : NULL;

      /* Completa uno split verticale preparato dentro la colonna. */
      if (parent && parent->children && parent->children->len == 1 &&
          parent->layout == FOLLETTO_LAYOUT_SPLITV &&
          !node_is_tab_container (parent))
        {
          insert_as_sibling (tree, target, leaf);
        }
      else
        {
          col = target ? niri_column_node (target) : NULL;
          leaf->parent = strip;
          if (!col)
            {
              g_ptr_array_add (strip->children, leaf);
            }
          else
            {
              for (i = 0; i < strip->children->len; i++)
                {
                  if (g_ptr_array_index (strip->children, i) == col)
                    {
                      idx = i + 1;
                      break;
                    }
                }
              g_ptr_array_insert (strip->children, (int) idx, leaf);
            }
          tree->focus = leaf;
        }
    }
  else if (wm->next_layout == FOLLETTO_LAYOUT_TABBED ||
           wm->next_layout == FOLLETTO_LAYOUT_STACKED)
    {
      FollettoLayout tabs_layout = wm->next_layout;
      FollettoNode *tabs;

      if (!tree->root || !node_is_tab_container (tree->root) ||
          tree->root->layout != tabs_layout)
        rebuild_as_tabs (tree, tabs_layout);

      tabs = tree->root;
      if (!tabs || !node_is_tab_container (tabs))
        {
          FollettoNode *wrap = node_new_split (tabs_layout);

          if (tree->root)
            {
              tree->root->parent = wrap;
              tree->root->percent = 1.0;
              g_ptr_array_add (wrap->children, tree->root);
            }
          tree->root = wrap;
          tabs = wrap;
        }

      leaf->parent = tabs;
      leaf->percent = 1.0;
      g_ptr_array_add (tabs->children, leaf);
      normalize_percents (tabs);
      tree->focus = leaf;
      /* Focus subito sulla nuova tab, così il layout non la nasconde. */
      meta_window_activate (window,
                            meta_display_get_current_time_roundtrip (wm->display));
    }
  else if (wm->next_layout == FOLLETTO_LAYOUT_GRID)
    {
      FollettoNode *target = (focus && !focus->floating) ? focus
                                                         : first_leaf (tree->root);
      FollettoNode *parent = target ? target->parent : NULL;

      /* Completa uno split esplicito (container con un solo figlio). */
      if (parent && parent->children && parent->children->len == 1 &&
          (parent->layout == FOLLETTO_LAYOUT_SPLITH ||
           parent->layout == FOLLETTO_LAYOUT_SPLITV) &&
          !node_is_tab_container (parent))
        {
          insert_as_sibling (tree, target, leaf);
        }
      else
        {
          /* 2ª: metà; 3ª: spezza la 2ª in verticale → 2ª sopra, 3ª sotto; ecc. */
          insert_binary_split (tree, target, leaf, grid_split_for_focus (target));
        }
    }
  else if (!focus || !focus->parent)
    {
      insert_binary_split (tree, focus, leaf, wm->next_layout);
    }
  else
    {
      insert_as_sibling (tree, focus, leaf);
    }

  g_signal_connect_swapped (window, "unmanaging",
                            G_CALLBACK (folletto_remove_window), wm);
  queue_layout (wm);
}

static void
flatten_if_single (FollettoTree *tree,
                   FollettoNode *parent)
{
  FollettoNode *only;

  if (!parent || !parent->children || parent->children->len != 1)
    return;
  /* Mantieni strip Niri e container tabbed/stacked anche con un solo figlio. */
  if (parent->layout == FOLLETTO_LAYOUT_NIRI ||
      node_is_tab_container (parent))
    return;
  only = g_ptr_array_index (parent->children, 0);
  if (parent == tree->root)
    {
      only->parent = NULL;
      g_ptr_array_free (parent->children, TRUE);
      g_free (parent);
      tree->root = only;
    }
  else
    {
      replace_in_parent (parent, only);
      g_ptr_array_free (parent->children, TRUE);
      g_free (parent);
    }
}

static void
folletto_remove_window (FollettoWm *wm,
                        MetaWindow *window)
{
  FollettoNode *node;
  FollettoTree *tree;
  FollettoNode *parent;
  guint i;

  node = g_hash_table_lookup (wm->nodes, window);
  if (!node)
    return;

  tree = tree_for_window (wm, window);
  parent = node->parent;

  if (window)
    window->layout_owned = FALSE;

  g_hash_table_remove (wm->nodes, window);

  if (tree->focus == node)
    tree->focus = NULL;

  if (tree->root == node)
    {
      tree->root = NULL;
      g_free (node);
      return;
    }

  if (parent && parent->children)
    {
      for (i = 0; i < parent->children->len; i++)
        {
          if (g_ptr_array_index (parent->children, i) == node)
            {
              g_ptr_array_remove_index (parent->children, i);
              break;
            }
        }
      g_free (node);
      normalize_percents (parent);
      flatten_if_single (tree, parent);
      if (!tree->focus)
        tree->focus = first_leaf (tree->root);
    }
  else
    {
      g_free (node);
    }

  queue_layout (wm);
}

static void
on_window_shown (MetaWindow *window,
                 FollettoWm *wm)
{
  g_signal_handlers_disconnect_by_func (window, on_window_shown, wm);
  if (!META_IS_WINDOW (window) || window->unmanaging)
    return;
  folletto_insert_window (wm, window);
}

static void
on_window_created (MetaDisplay *display,
                   MetaWindow  *window,
                   FollettoWm  *wm)
{
  if (window->mapped)
    folletto_insert_window (wm, window);
  else
    g_signal_connect (window, "shown", G_CALLBACK (on_window_shown), wm);
}

static void
release_all_layout (FollettoWm *wm)
{
  GHashTableIter iter;
  MetaWindow *window;

  g_hash_table_iter_init (&iter, wm->nodes);
  while (g_hash_table_iter_next (&iter, (gpointer *) &window, NULL))
    {
      if (!window)
        continue;
      window->layout_owned = FALSE;
      apply_chrome (wm, window, FALSE);
    }
}

static void
capture_existing_windows (FollettoWm *wm)
{
  GSList *list, *l;

  list = meta_display_list_windows (wm->display, META_LIST_DEFAULT);
  for (l = list; l; l = l->next)
    folletto_insert_window (wm, l->data);
  g_slist_free (list);
}

static void
cmd_focus_dir (FollettoWm         *wm,
               MetaDisplayDirection dir)
{
  FollettoNode *leaf = focused_leaf (wm);
  FollettoNode *n, *parent;
  guint i, idx = 0;
  gboolean horizontal = (dir == META_DISPLAY_LEFT || dir == META_DISPLAY_RIGHT);
  gboolean forward = (dir == META_DISPLAY_RIGHT || dir == META_DISPLAY_DOWN);

  if (!leaf)
    return;

  n = leaf;
  while (n->parent)
    {
      parent = n->parent;
      if ((horizontal && (parent->layout == FOLLETTO_LAYOUT_SPLITH ||
                          parent->layout == FOLLETTO_LAYOUT_NIRI)) ||
          (!horizontal && parent->layout == FOLLETTO_LAYOUT_SPLITV) ||
          parent->layout == FOLLETTO_LAYOUT_TABBED ||
          parent->layout == FOLLETTO_LAYOUT_STACKED)
        {
          for (i = 0; i < parent->children->len; i++)
            {
              if (g_ptr_array_index (parent->children, i) == n)
                {
                  idx = i;
                  break;
                }
            }
          if (forward && idx + 1 < parent->children->len)
            {
              FollettoNode *dest = first_leaf (g_ptr_array_index (parent->children, idx + 1));
              FollettoTree *tree = focused_tree (wm);

              if (dest && dest->window)
                {
                  tree->focus = dest;
                  meta_window_activate (dest->window,
                                        meta_display_get_current_time (wm->display));
                  if (tree_is_niri (wm, tree))
                    folletto_layout_tree (wm, tree);
                }
              return;
            }
          if (!forward && idx > 0)
            {
              FollettoNode *dest = first_leaf (g_ptr_array_index (parent->children, idx - 1));
              FollettoTree *tree = focused_tree (wm);

              if (dest && dest->window)
                {
                  tree->focus = dest;
                  meta_window_activate (dest->window,
                                        meta_display_get_current_time (wm->display));
                  if (tree_is_niri (wm, tree))
                    folletto_layout_tree (wm, tree);
                }
              return;
            }
        }
      n = parent;
    }
}

static void
cmd_move_dir (FollettoWm          *wm,
              MetaDisplayDirection dir)
{
  FollettoNode *leaf = focused_leaf (wm);
  FollettoNode *n, *parent, *sib;
  guint i, idx = 0;
  gboolean horizontal = (dir == META_DISPLAY_LEFT || dir == META_DISPLAY_RIGHT);
  gboolean forward = (dir == META_DISPLAY_RIGHT || dir == META_DISPLAY_DOWN);

  if (!leaf || !leaf->parent)
    return;

  n = leaf;
  parent = n->parent;
  while (parent)
    {
      if ((horizontal && (parent->layout == FOLLETTO_LAYOUT_SPLITH ||
                          parent->layout == FOLLETTO_LAYOUT_NIRI)) ||
          (!horizontal && parent->layout == FOLLETTO_LAYOUT_SPLITV))
        {
          for (i = 0; i < parent->children->len; i++)
            {
              if (g_ptr_array_index (parent->children, i) == n)
                {
                  idx = i;
                  break;
                }
            }
          if (forward && idx + 1 < parent->children->len)
            {
              sib = g_ptr_array_index (parent->children, idx + 1);
              g_ptr_array_index (parent->children, idx + 1) = n;
              g_ptr_array_index (parent->children, idx) = sib;
              folletto_layout_tree (wm, focused_tree (wm));
              return;
            }
          if (!forward && idx > 0)
            {
              sib = g_ptr_array_index (parent->children, idx - 1);
              g_ptr_array_index (parent->children, idx - 1) = n;
              g_ptr_array_index (parent->children, idx) = sib;
              folletto_layout_tree (wm, focused_tree (wm));
              return;
            }
        }
      n = parent;
      parent = n->parent;
    }
}

static void
cmd_split (FollettoWm    *wm,
           FollettoLayout layout)
{
  FollettoNode *leaf = focused_leaf (wm);
  FollettoNode *split;
  FollettoTree *tree;

  /* Griglia/Niri: lo split esplicito prepara il prossimo slot senza cambiare modalità. */
  if (wm->next_layout != FOLLETTO_LAYOUT_GRID &&
      wm->next_layout != FOLLETTO_LAYOUT_NIRI)
    wm->next_layout = layout;
  if (!leaf)
    return;

  tree = focused_tree (wm);

  /* In Niri, split h = nuova colonna (noop fino alla prossima finestra via insert);
   * split v = impila nella colonna corrente. */
  if (wm->next_layout == FOLLETTO_LAYOUT_NIRI &&
      layout == FOLLETTO_LAYOUT_SPLITH)
    return;

  split = node_new_split (geometry_layout (layout));
  if (tree->root == leaf)
    {
      split->parent = NULL;
      leaf->parent = split;
      leaf->percent = 1.0;
      g_ptr_array_add (split->children, leaf);
      tree->root = split;
    }
  else
    {
      FollettoNode *col = niri_column_node (leaf);

      if (wm->next_layout == FOLLETTO_LAYOUT_NIRI &&
          layout == FOLLETTO_LAYOUT_SPLITV && col &&
          col->parent && col->parent->layout == FOLLETTO_LAYOUT_NIRI &&
          node_is_leaf (col))
        {
          /* Sostituisci la colonna-foglia con uno stack verticale. */
          split->percent = col->percent;
          replace_in_parent (col, split);
          col->parent = split;
          col->percent = 1.0;
          g_ptr_array_add (split->children, col);
          return;
        }

      replace_in_parent (leaf, split);
      leaf->parent = split;
      leaf->percent = 1.0;
      g_ptr_array_add (split->children, leaf);
    }
}

static void
cmd_layout (FollettoWm    *wm,
            FollettoLayout layout)
{
  FollettoNode *leaf = focused_leaf (wm);
  FollettoNode *n;
  FollettoTree *tree = focused_tree (wm);

  unfloat_tree (tree);
  if (!leaf)
    leaf = focused_leaf (wm);

  wm->next_layout = layout;

  /* Griglia: ricostruisce l’albero con split binari H/V alternati. */
  if (layout == FOLLETTO_LAYOUT_GRID)
    {
      if (tree)
        {
          rebuild_as_grid (tree);
          folletto_layout_tree (wm, tree);
        }
      emit_layout_changed (wm);
      return;
    }

  if (layout == FOLLETTO_LAYOUT_NIRI)
    {
      if (tree)
        {
          rebuild_as_niri (tree);
          folletto_layout_tree (wm, tree);
        }
      emit_layout_changed (wm);
      return;
    }

  if (layout == FOLLETTO_LAYOUT_TABBED || layout == FOLLETTO_LAYOUT_STACKED)
    {
      if (tree)
        {
          rebuild_as_tabs (tree, layout);
          folletto_layout_tree (wm, tree);
        }
      emit_layout_changed (wm);
      return;
    }

  if (!leaf)
    {
      emit_layout_changed (wm);
      return;
    }
  n = leaf->parent ? leaf->parent : leaf;
  /* Non sovrascrivere lo strip Niri con splith/splitv “locali” se si esce da niri:
   * rebuild avviene solo entrando; uscendo si riassegna il container del focus. */
  if (n->layout == FOLLETTO_LAYOUT_NIRI &&
      (layout == FOLLETTO_LAYOUT_SPLITH || layout == FOLLETTO_LAYOUT_SPLITV))
    {
      FollettoNode *strip = n;
      FollettoNode *flat = node_new_split (layout);
      guint i;

      for (i = 0; i < strip->children->len; i++)
        {
          FollettoNode *ch = g_ptr_array_index (strip->children, i);

          ch->parent = flat;
          ch->percent = 1.0 / MAX (strip->children->len, 1);
          g_ptr_array_add (flat->children, ch);
        }
      g_ptr_array_set_size (strip->children, 0);
      g_ptr_array_free (strip->children, TRUE);
      g_free (strip);
      tree->root = flat;
      tree->view_offset = 0;
      normalize_percents (flat);
      folletto_layout_tree (wm, tree);
      emit_layout_changed (wm);
      return;
    }
  n->layout = layout;
  folletto_layout_tree (wm, tree);
  emit_layout_changed (wm);
}

static void
cmd_scroll_delta (FollettoWm *wm,
                  double      dx)
{
  FollettoTree *tree = focused_tree (wm);

  if (!tree || !tree_is_niri (wm, tree))
    return;
  tree->view_offset += dx;
  folletto_layout_tree (wm, tree);
}

static void
on_focus_window_niri (MetaDisplay *display,
                      GParamSpec  *pspec,
                      FollettoWm  *wm)
{
  (void) display;
  (void) pspec;
  if (!wm || wm->mode != META_WM_MODE_I3 ||
      wm->next_layout != FOLLETTO_LAYOUT_NIRI)
    return;
  queue_layout (wm);
}

static void
cmd_workspace_floating (FollettoWm *wm)
{
  FollettoTree *tree = focused_tree (wm);

  if (!tree || !tree->root)
    return;
  foreach_leaf (tree->root, set_leaf_floating, GINT_TO_POINTER (TRUE));
  folletto_layout_tree (wm, tree);
  emit_layout_changed (wm);
}

static void
cmd_floating_toggle (FollettoWm *wm)
{
  FollettoNode *leaf = focused_leaf (wm);

  if (!leaf || !leaf->window)
    return;
  leaf->floating = !leaf->floating;
  leaf->window->layout_owned = !leaf->floating;
  apply_chrome (wm, leaf->window, !leaf->floating);
  folletto_layout_tree (wm, focused_tree (wm));
  emit_layout_changed (wm);
}

static void
cmd_fullscreen (FollettoWm *wm)
{
  FollettoNode *leaf = focused_leaf (wm);

  if (!leaf || !leaf->window)
    return;
  if (meta_window_is_fullscreen (leaf->window))
    meta_window_unmake_fullscreen (leaf->window);
  else
    meta_window_make_fullscreen (leaf->window);
}

static void
cmd_kill (FollettoWm *wm)
{
  FollettoNode *leaf = focused_leaf (wm);

  if (!leaf || !leaf->window)
    return;
  meta_window_delete (leaf->window, meta_display_get_current_time (wm->display));
}

static void
cmd_workspace (FollettoWm *wm,
               int         index)
{
  MetaWorkspaceManager *wsm = meta_display_get_workspace_manager (wm->display);
  MetaWorkspace *ws;

  if (index < 0)
    return;
  if (meta_prefs_get_num_workspaces () < 10)
    meta_prefs_set_num_workspaces (10);
  ws = meta_workspace_manager_get_workspace_by_index (wsm, index);
  if (ws)
    meta_workspace_activate (ws, meta_display_get_current_time (wm->display));
}

static void
cmd_move_workspace (FollettoWm *wm,
                    int         index)
{
  FollettoNode *leaf = focused_leaf (wm);
  MetaWindow *window;

  if (!leaf || !leaf->window)
    return;
  window = leaf->window;
  folletto_remove_window (wm, window);
  if (meta_prefs_get_num_workspaces () < 10)
    meta_prefs_set_num_workspaces (10);
  meta_window_change_workspace_by_index (window, index, FALSE);
  folletto_insert_window (wm, window);
}

static void
cmd_resize (FollettoWm *wm,
            gboolean    grow,
            gboolean    width,
            double      ppt)
{
  FollettoNode *leaf = focused_leaf (wm);
  FollettoNode *n, *parent;
  FollettoLayout want = width ? FOLLETTO_LAYOUT_SPLITH : FOLLETTO_LAYOUT_SPLITV;
  guint i, idx = 0;
  FollettoNode *sib;
  double delta = ppt / 100.0;
  FollettoTree *tree;

  if (!leaf)
    return;

  tree = focused_tree (wm);
  if (tree_is_niri (wm, tree) && width)
    {
      FollettoNode *col = niri_column_node (leaf);

      if (!grow)
        delta = -delta;
      /* Larghezza colonna in frazione di schermo (le altre non si rimpiccioliscono). */
      col->percent = CLAMP (col->percent + delta, 0.15, 1.0);
      folletto_layout_tree (wm, tree);
      return;
    }

  n = leaf;
  while (n->parent)
    {
      parent = n->parent;
      if (parent->layout == want && parent->children->len >= 2)
        {
          for (i = 0; i < parent->children->len; i++)
            {
              if (g_ptr_array_index (parent->children, i) == n)
                {
                  idx = i;
                  break;
                }
            }
          sib = g_ptr_array_index (parent->children,
                                   (idx + 1 < parent->children->len) ? idx + 1 : idx - 1);
          if (!grow)
            delta = -delta;
          n->percent = CLAMP (n->percent + delta, 0.05, 0.95);
          sib->percent = CLAMP (sib->percent - delta, 0.05, 0.95);
          normalize_percents (parent);
          folletto_layout_tree (wm, focused_tree (wm));
          return;
        }
      n = parent;
    }
}

static void
cmd_scratchpad_move (FollettoWm *wm)
{
  FollettoNode *leaf = focused_leaf (wm);

  if (!leaf || !leaf->window)
    return;
  leaf->floating = TRUE;
  leaf->window->layout_owned = FALSE;
  apply_chrome (wm, leaf->window, FALSE);
  g_ptr_array_add (wm->scratchpad, leaf->window);
  meta_window_minimize (leaf->window);
  folletto_layout_tree (wm, focused_tree (wm));
}

static void
cmd_scratchpad_show (FollettoWm *wm)
{
  MetaWindow *window;

  if (wm->scratchpad->len == 0)
    return;
  window = g_ptr_array_index (wm->scratchpad, wm->scratchpad->len - 1);
  g_ptr_array_remove_index (wm->scratchpad, wm->scratchpad->len - 1);
  meta_window_unminimize (window);
  meta_window_activate (window, meta_display_get_current_time (wm->display));
}

static void
cmd_focus_id (FollettoWm *wm,
              guint64     id)
{
  GHashTableIter iter;
  MetaWindow *window;
  FollettoNode *node;

  g_hash_table_iter_init (&iter, wm->nodes);
  while (g_hash_table_iter_next (&iter, (gpointer *) &window,
                                 (gpointer *) &node))
    {
      if (!window || meta_window_get_id (window) != id)
        continue;
      meta_window_activate (window, meta_display_get_current_time (wm->display));
      meta_window_raise (window);
      tree_for_window (wm, window)->focus = node;
      folletto_layout_tree (wm, tree_for_window (wm, window));
      emit_layout_changed (wm);
      return;
    }
}

static MetaDisplayDirection
parse_dir (const char *s)
{
  if (g_str_equal (s, "left") || g_str_equal (s, "h"))
    return META_DISPLAY_LEFT;
  if (g_str_equal (s, "right"))
    return META_DISPLAY_RIGHT;
  if (g_str_equal (s, "up") || g_str_equal (s, "k"))
    return META_DISPLAY_UP;
  return META_DISPLAY_DOWN;
}

gboolean
folletto_wm_command (FollettoWm *wm,
                     const char *command)
{
  char **parts;
  char *cmd;
  gboolean ok = TRUE;

  if (!wm || !command)
    return FALSE;

  wm->drag_window = NULL;

  cmd = g_strdup (command);
  g_strstrip (cmd);
  parts = g_strsplit_set (cmd, " \t", -1);

  if (!parts[0] || parts[0][0] == '\0')
    {
      ok = FALSE;
    }
  else if (g_str_equal (parts[0], "focusid") && parts[1])
    {
      cmd_focus_id (wm, g_ascii_strtoull (parts[1], NULL, 10));
    }
  else if (g_str_equal (parts[0], "focus") && parts[1])
    {
      if (g_str_equal (parts[1], "parent"))
        {
          FollettoNode *leaf = focused_leaf (wm);
          if (leaf && leaf->parent && leaf->parent->parent)
            focused_tree (wm)->focus = first_leaf (leaf->parent);
        }
      else if (g_str_equal (parts[1], "child"))
        {
          /* stay on leaf */
        }
      else
        {
          cmd_focus_dir (wm, parse_dir (parts[1]));
        }
    }
  else if (g_str_equal (parts[0], "move") && parts[1])
    {
      if (g_str_equal (parts[1], "scratchpad"))
        cmd_scratchpad_move (wm);
      else if (g_str_equal (parts[1], "workspace") && parts[2] &&
               g_str_equal (parts[2], "number") && parts[3])
        cmd_move_workspace (wm, atoi (parts[3]) - 1);
      else
        cmd_move_dir (wm, parse_dir (parts[1]));
    }
  else if (g_str_equal (parts[0], "workspace") && parts[1])
    {
      if (g_str_equal (parts[1], "number") && parts[2])
        cmd_workspace (wm, atoi (parts[2]) - 1);
      else if (g_ascii_isdigit (parts[1][0]))
        cmd_workspace (wm, atoi (parts[1]) - 1);
    }
  else if (g_str_equal (parts[0], "split"))
    {
      if (!parts[1] || g_str_equal (parts[1], "h") ||
          g_str_equal (parts[1], "horizontal"))
        cmd_split (wm, FOLLETTO_LAYOUT_SPLITH);
      else if (g_str_equal (parts[1], "toggle"))
        cmd_split (wm, wm->next_layout == FOLLETTO_LAYOUT_SPLITH
                   ? FOLLETTO_LAYOUT_SPLITV : FOLLETTO_LAYOUT_SPLITH);
      else
        cmd_split (wm, FOLLETTO_LAYOUT_SPLITV);
    }
  else if (g_str_equal (parts[0], "layout") && parts[1])
    {
      if (g_str_equal (parts[1], "tabbed"))
        cmd_layout (wm, FOLLETTO_LAYOUT_TABBED);
      else if (g_str_equal (parts[1], "stacked"))
        cmd_layout (wm, FOLLETTO_LAYOUT_STACKED);
      else if (g_str_equal (parts[1], "grid") || g_str_equal (parts[1], "griglia") ||
               g_str_equal (parts[1], "default") || g_str_equal (parts[1], "tiling"))
        cmd_layout (wm, FOLLETTO_LAYOUT_GRID);
      else if (g_str_equal (parts[1], "niri"))
        cmd_layout (wm, FOLLETTO_LAYOUT_NIRI);
      else if (g_str_equal (parts[1], "splith"))
        cmd_layout (wm, FOLLETTO_LAYOUT_SPLITH);
      else if (g_str_equal (parts[1], "splitv"))
        cmd_layout (wm, FOLLETTO_LAYOUT_SPLITV);
      else if (g_str_equal (parts[1], "floating"))
        cmd_workspace_floating (wm);
      else if (g_str_equal (parts[1], "toggle"))
        {
          FollettoNode *leaf = focused_leaf (wm);
          FollettoNode *n = (leaf && leaf->parent) ? leaf->parent : leaf;
          if (n)
            cmd_layout (wm, n->layout == FOLLETTO_LAYOUT_SPLITH
                        ? FOLLETTO_LAYOUT_SPLITV : FOLLETTO_LAYOUT_SPLITH);
        }
    }
  else if (g_str_equal (parts[0], "scroll"))
    {
      if (parts[1] && g_str_equal (parts[1], "delta") && parts[2])
        cmd_scroll_delta (wm, g_ascii_strtod (parts[2], NULL));
      else if (parts[1] && g_str_equal (parts[1], "left"))
        cmd_scroll_delta (wm, -120);
      else if (parts[1] && g_str_equal (parts[1], "right"))
        cmd_scroll_delta (wm, 120);
      else
        ok = FALSE;
    }
  else if (g_str_equal (parts[0], "floating"))
    {
      cmd_floating_toggle (wm);
    }
  else if (g_str_equal (parts[0], "fullscreen"))
    {
      cmd_fullscreen (wm);
    }
  else if (g_str_equal (parts[0], "kill"))
    {
      cmd_kill (wm);
    }
  else if (g_str_equal (parts[0], "resize") && parts[1] && parts[2] && parts[3])
    {
      gboolean grow = g_str_equal (parts[1], "grow");
      gboolean width = g_str_equal (parts[2], "width");
      cmd_resize (wm, grow, width, g_ascii_strtod (parts[3], NULL));
    }
  else if (g_str_equal (parts[0], "gaps") && parts[1] && parts[2])
    {
      int v = atoi (parts[parts[3] ? 3 : 2]);
      if (g_str_has_prefix (parts[1], "inner"))
        wm->gaps_inner = v;
      else
        wm->gaps_outer = v;
      folletto_layout_tree (wm, focused_tree (wm));
    }
  else if (g_str_equal (parts[0], "mode") && parts[1])
    {
      wm->resize_mode = g_str_equal (parts[1], "resize");
    }
  else if (g_str_equal (parts[0], "scratchpad") && parts[1] &&
           g_str_equal (parts[1], "show"))
    {
      cmd_scratchpad_show (wm);
    }
  else if (g_str_equal (parts[0], "toggle_mode"))
    {
      folletto_wm_set_mode (wm, wm->mode == META_WM_MODE_I3
                            ? META_WM_MODE_NORMAL : META_WM_MODE_I3);
    }
  else
    {
      ok = FALSE;
    }

  g_strfreev (parts);
  g_free (cmd);
  return ok;
}

static void
on_i3_binding (MetaDisplay        *display,
               MetaWindow         *window,
               const ClutterEvent *event,
               MetaKeyBinding     *binding,
               gpointer            user_data)
{
  FollettoWm *wm = user_data;
  const char *name = meta_key_binding_get_name (binding);
  char *command = NULL;
  int n;

  (void) display;
  (void) window;
  (void) event;

  if (g_str_equal (name, "toggle-mode"))
    command = g_strdup ("toggle_mode");
  else if (wm->mode != META_WM_MODE_I3)
    return;
  else if (g_str_equal (name, "focus-left"))
    command = g_strdup ("focus left");
  else if (g_str_equal (name, "focus-right"))
    command = g_strdup ("focus right");
  else if (g_str_equal (name, "focus-up"))
    command = g_strdup ("focus up");
  else if (g_str_equal (name, "focus-down"))
    command = g_strdup ("focus down");
  else if (g_str_equal (name, "move-left"))
    command = g_strdup ("move left");
  else if (g_str_equal (name, "move-right"))
    command = g_strdup ("move right");
  else if (g_str_equal (name, "move-up"))
    command = g_strdup ("move up");
  else if (g_str_equal (name, "move-down"))
    command = g_strdup ("move down");
  else if (g_str_equal (name, "split-h"))
    command = g_strdup ("split h");
  else if (g_str_equal (name, "split-v"))
    command = g_strdup ("split v");
  else if (g_str_equal (name, "layout-tabbed"))
    command = g_strdup ("layout tabbed");
  else if (g_str_equal (name, "layout-stacked"))
    command = g_strdup ("layout stacked");
  else if (g_str_equal (name, "layout-grid"))
    command = g_strdup ("layout grid");
  else if (g_str_equal (name, "layout-niri"))
    command = g_strdup ("layout niri");
  else if (g_str_equal (name, "layout-splith"))
    command = g_strdup ("layout splith");
  else if (g_str_equal (name, "layout-splitv"))
    command = g_strdup ("layout splitv");
  else if (g_str_equal (name, "layout-toggle-split"))
    command = g_strdup ("layout toggle split");
  else if (g_str_equal (name, "scroll-left"))
    command = g_strdup ("scroll left");
  else if (g_str_equal (name, "scroll-right"))
    command = g_strdup ("scroll right");
  else if (g_str_equal (name, "floating-toggle"))
    command = g_strdup ("floating toggle");
  else if (g_str_equal (name, "fullscreen"))
    command = g_strdup ("fullscreen toggle");
  else if (g_str_equal (name, "kill"))
    command = g_strdup ("kill");
  else if (g_str_equal (name, "scratchpad-move"))
    command = g_strdup ("move scratchpad");
  else if (g_str_equal (name, "scratchpad-show"))
    command = g_strdup ("scratchpad show");
  else if (g_str_has_prefix (name, "workspace-") &&
           sscanf (name, "workspace-%d", &n) == 1)
    command = g_strdup_printf ("workspace number %d", n);
  else if (g_str_has_prefix (name, "move-workspace-") &&
           sscanf (name, "move-workspace-%d", &n) == 1)
    command = g_strdup_printf ("move workspace number %d", n);
  else if (wm->resize_mode && g_str_equal (name, "resize-grow-width"))
    command = g_strdup ("resize grow width 5 ppt");
  else if (wm->resize_mode && g_str_equal (name, "resize-shrink-width"))
    command = g_strdup ("resize shrink width 5 ppt");
  else if (g_str_equal (name, "mode-resize"))
    command = g_strdup ("mode resize");
  else if (g_str_equal (name, "mode-default"))
    command = g_strdup ("mode default");

  if (command)
    {
      folletto_wm_command (wm, command);
      g_free (command);
    }
}

static const char *i3_binding_names[] = {
  "focus-left", "focus-right", "focus-up", "focus-down",
  "move-left", "move-right", "move-up", "move-down",
  "split-h", "split-v",
  "layout-grid", "layout-niri", "layout-tabbed", "layout-stacked", "layout-splith", "layout-splitv",
  "layout-toggle-split",
  "scroll-left", "scroll-right",
  "floating-toggle", "fullscreen", "kill",
  "scratchpad-move", "scratchpad-show",
  "workspace-1", "workspace-2", "workspace-3", "workspace-4", "workspace-5",
  "workspace-6", "workspace-7", "workspace-8", "workspace-9", "workspace-10",
  "move-workspace-1", "move-workspace-2", "move-workspace-3", "move-workspace-4",
  "move-workspace-5", "move-workspace-6", "move-workspace-7", "move-workspace-8",
  "move-workspace-9", "move-workspace-10",
  "mode-resize", "mode-default",
  "resize-grow-width", "resize-shrink-width",
  NULL
};

static void
folletto_grab_i3_keys (FollettoWm *wm)
{
  guint i;

  if (!wm->settings)
    return;
  for (i = 0; i3_binding_names[i]; i++)
    {
      meta_display_add_keybinding (wm->display, i3_binding_names[i],
                                   wm->settings,
                                   META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                   on_i3_binding, wm, NULL);
      g_ptr_array_add (wm->grabbed_bindings, g_strdup (i3_binding_names[i]));
    }
}

static void
folletto_ungrab_i3_keys (FollettoWm *wm)
{
  guint i;

  for (i = 0; i < wm->grabbed_bindings->len; i++)
    {
      const char *name = g_ptr_array_index (wm->grabbed_bindings, i);
      meta_display_remove_keybinding (wm->display, name);
    }
  g_ptr_array_set_size (wm->grabbed_bindings, 0);
}

void
folletto_wm_set_mode (FollettoWm *wm,
                      MetaWmMode  mode)
{
  if (!wm || wm->mode == mode)
    return;

  if (mode == META_WM_MODE_I3)
    {
      GHashTableIter iter;
      FollettoNode *node;
      FollettoTree *tree;

      wm->mode = mode;
      if (meta_prefs_get_num_workspaces () < 10)
        meta_prefs_set_num_workspaces (10);
      capture_existing_windows (wm);
      g_hash_table_iter_init (&iter, wm->nodes);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &node))
        {
          if (node->window && !node->floating)
            {
              node->window->layout_owned = TRUE;
              apply_chrome (wm, node->window, TRUE);
            }
        }
      g_hash_table_iter_init (&iter, wm->trees);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &tree))
        folletto_layout_tree (wm, tree);
      folletto_grab_i3_keys (wm);
    }
  else
    {
      folletto_ungrab_i3_keys (wm);
      release_all_layout (wm);
      wm->mode = mode;
    }

  if (wm->settings)
    g_settings_set_string (wm->settings, "mode",
                           mode == META_WM_MODE_I3 ? "i3" : "normal");

  g_signal_emit_by_name (wm->display, "wm-mode-changed", mode);
  emit_layout_changed (wm);
}

MetaWmMode
folletto_wm_get_mode (FollettoWm *wm)
{
  return wm ? wm->mode : META_WM_MODE_NORMAL;
}

FollettoWm *
folletto_wm_new (MetaDisplay *display)
{
  FollettoWm *wm = g_new0 (FollettoWm, 1);
  GSettingsSchema *schema;
  GSettingsSchemaSource *source;

  wm->display = display;
  wm->mode = META_WM_MODE_NORMAL;
  wm->next_layout = FOLLETTO_LAYOUT_GRID;
  wm->gaps_inner = 4;
  wm->gaps_outer = 4;
  wm->drag_window = NULL;
  wm->trees = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  wm->nodes = g_hash_table_new (NULL, NULL);
  wm->scratchpad = g_ptr_array_new ();
  wm->grabbed_bindings = g_ptr_array_new_with_free_func (g_free);

  source = g_settings_schema_source_get_default ();
  schema = g_settings_schema_source_lookup (source, "org.folletto.wm", TRUE);
  if (schema)
    {
      char *mode;

      wm->settings = g_settings_new ("org.folletto.wm");
      g_settings_schema_unref (schema);
      wm->show_decorations =
        g_settings_get_boolean (wm->settings, "show-decorations");
      wm->gaps_inner = g_settings_get_int (wm->settings, "gaps-inner");
      wm->gaps_outer = g_settings_get_int (wm->settings, "gaps-outer");
      {
        char *dl = g_settings_get_string (wm->settings, "default-layout");
        wm->next_layout = layout_from_name (dl);
        g_free (dl);
      }
      g_signal_connect (wm->settings, "changed",
                        G_CALLBACK (on_settings_changed), wm);
      mode = g_settings_get_string (wm->settings, "mode");
      meta_display_add_keybinding (display, "toggle-mode", wm->settings,
                                   META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                   on_i3_binding, wm, NULL);
      if (g_str_equal (mode, "i3"))
        folletto_wm_set_mode (wm, META_WM_MODE_I3);
      g_free (mode);
    }

  g_signal_connect (display, "window-created",
                    G_CALLBACK (on_window_created), wm);
  g_signal_connect (display, "grab-op-begin",
                    G_CALLBACK (on_grab_op_begin), wm);
  g_signal_connect (display, "grab-op-end",
                    G_CALLBACK (on_grab_op_end), wm);
  g_signal_connect (display, "notify::focus-window",
                    G_CALLBACK (on_focus_window_niri), wm);
  g_timeout_add (2000, folletto_wm_export_dbus_idle, wm);
  return wm;
}

void
folletto_wm_free (FollettoWm *wm)
{
  if (!wm)
    return;
  cancel_grab_job (wm);
  if (wm->layout_idle_id)
    g_source_remove (wm->layout_idle_id);
  if (wm->layout_emit_id)
    g_source_remove (wm->layout_emit_id);
  g_signal_handlers_disconnect_by_data (wm->display, wm);
  folletto_ungrab_i3_keys (wm);
  if (wm->dbus_conn && wm->dbus_reg_id)
    g_dbus_connection_unregister_object (wm->dbus_conn, wm->dbus_reg_id);
  if (wm->dbus_owner_id)
    g_bus_unown_name (wm->dbus_owner_id);
  g_clear_pointer (&wm->dbus_info, g_dbus_node_info_unref);
  g_clear_object (&wm->dbus_conn);
  g_clear_object (&wm->settings);
  g_hash_table_destroy (wm->trees);
  g_hash_table_destroy (wm->nodes);
  g_ptr_array_free (wm->scratchpad, TRUE);
  g_ptr_array_free (wm->grabbed_bindings, TRUE);
  g_free (wm);
}

META_EXPORT
void
meta_display_set_wm_mode (MetaDisplay *display,
                          MetaWmMode   mode)
{
  g_return_if_fail (META_IS_DISPLAY (display));
  if (display->folletto_wm)
    folletto_wm_set_mode (display->folletto_wm, mode);
}

META_EXPORT
MetaWmMode
meta_display_get_wm_mode (MetaDisplay *display)
{
  g_return_val_if_fail (META_IS_DISPLAY (display), META_WM_MODE_NORMAL);
  if (!display->folletto_wm)
    return META_WM_MODE_NORMAL;
  return folletto_wm_get_mode (display->folletto_wm);
}

META_EXPORT
gboolean
meta_display_i3_command (MetaDisplay *display,
                         const char  *command)
{
  g_return_val_if_fail (META_IS_DISPLAY (display), FALSE);
  if (!display->folletto_wm)
    return FALSE;
  return folletto_wm_command (display->folletto_wm, command);
}

META_EXPORT
const char *
meta_display_get_i3_layout (MetaDisplay *display)
{
  g_return_val_if_fail (META_IS_DISPLAY (display), "normal");
  if (!display->folletto_wm)
    return "normal";
  return folletto_wm_get_layout (display->folletto_wm);
}

static void
relayout_all (FollettoWm *wm)
{
  GHashTableIter iter;
  FollettoTree *tree;

  if (wm->mode != META_WM_MODE_I3)
    return;
  emit_layout_changed (wm);
  g_hash_table_iter_init (&iter, wm->trees);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &tree))
    folletto_layout_tree (wm, tree);
}

static gboolean
layout_all_idle (gpointer data)
{
  FollettoWm *wm = data;

  wm->layout_idle_id = 0;
  if (wm->mode != META_WM_MODE_I3 || wm->drag_window)
    return G_SOURCE_REMOVE;
  relayout_all (wm);
  return G_SOURCE_REMOVE;
}

static void
queue_layout (FollettoWm *wm)
{
  if (!wm || wm->layout_idle_id)
    return;
  wm->layout_idle_id = g_timeout_add (80, layout_all_idle, wm);
}

static void
on_show_decorations_changed (GSettings  *settings,
                             gchar      *key,
                             FollettoWm *wm)
{
  (void) key;
  wm->show_decorations = g_settings_get_boolean (settings, "show-decorations");
  relayout_all (wm);
}

static FollettoLayout
layout_from_name (const char *name)
{
  if (!name)
    return FOLLETTO_LAYOUT_GRID;
  if (g_str_equal (name, "grid") || g_str_equal (name, "griglia") ||
      g_str_equal (name, "default") || g_str_equal (name, "tiling"))
    return FOLLETTO_LAYOUT_GRID;
  if (g_str_equal (name, "niri"))
    return FOLLETTO_LAYOUT_NIRI;
  if (g_str_equal (name, "splith"))
    return FOLLETTO_LAYOUT_SPLITH;
  if (g_str_equal (name, "splitv"))
    return FOLLETTO_LAYOUT_SPLITV;
  if (g_str_equal (name, "tabbed"))
    return FOLLETTO_LAYOUT_TABBED;
  if (g_str_equal (name, "stacked"))
    return FOLLETTO_LAYOUT_STACKED;
  return FOLLETTO_LAYOUT_GRID;
}

static void
on_settings_changed (GSettings  *settings,
                     gchar      *key,
                     FollettoWm *wm)
{
  if (g_str_equal (key, "show-decorations"))
    {
      on_show_decorations_changed (settings, key, wm);
      return;
    }
  if (g_str_equal (key, "gaps-inner"))
    {
      wm->gaps_inner = g_settings_get_int (settings, "gaps-inner");
      relayout_all (wm);
      return;
    }
  if (g_str_equal (key, "gaps-outer"))
    {
      wm->gaps_outer = g_settings_get_int (settings, "gaps-outer");
      relayout_all (wm);
      return;
    }
  if (g_str_equal (key, "default-layout"))
    {
      char *name = g_settings_get_string (settings, "default-layout");
      wm->next_layout = layout_from_name (name);
      g_free (name);
      return;
    }
}

static void
collect_tile (FollettoNode *n,
              gpointer      data)
{
  GVariantBuilder *builder = data;

  if (n->floating || !n->window)
    return;
  g_variant_builder_add (builder, "(iiii)",
                         n->cell.x, n->cell.y,
                         n->cell.width, n->cell.height);
}

META_EXPORT
GVariant *
meta_display_get_i3_tiles (MetaDisplay *display)
{
  GVariantBuilder builder;
  FollettoWm *wm;
  FollettoTree *tree;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiii)"));
  if (!META_IS_DISPLAY (display) || !display->folletto_wm)
    return g_variant_builder_end (&builder);

  wm = display->folletto_wm;
  if (wm->mode != META_WM_MODE_I3 || wm->layout_busy)
    return g_variant_builder_end (&builder);

  tree = focused_tree (wm);
  if (tree && tree->root)
    foreach_leaf (tree->root, collect_tile, &builder);
  return g_variant_builder_end (&builder);
}

static void
add_chrome_tabs (FollettoNode    *n,
                 GVariantBuilder *tabs,
                 MetaWindow      *focus)
{
  guint i;
  const char *title;
  guint64 id;
  gboolean focused;

  if (!n)
    return;
  if (node_is_leaf (n))
    {
      title = (n->window && meta_window_get_title (n->window))
              ? meta_window_get_title (n->window) : "";
      id = n->window ? meta_window_get_id (n->window) : 0;
      focused = n->window && n->window == focus;
      g_variant_builder_add (tabs, "(tsb)", id, title, focused);
      return;
    }
  if (!n->children)
    return;
  for (i = 0; i < n->children->len; i++)
    add_chrome_tabs (g_ptr_array_index (n->children, i), tabs, focus);
}

static void
collect_chrome_node (FollettoNode    *n,
                     GVariantBuilder *builder,
                     MetaWindow      *focus)
{
  guint i;

  if (!n)
    return;

  if (node_is_tab_container (n) &&
      (node_is_leaf (n) || (n->children && n->children->len > 0)))
    {
      GVariantBuilder tabs;

      g_variant_builder_init (&tabs, G_VARIANT_TYPE ("a(tsb)"));
      add_chrome_tabs (n, &tabs, focus);
      g_variant_builder_open (builder, G_VARIANT_TYPE ("(siiiia(tsb))"));
      g_variant_builder_add (builder, "s", layout_name (n->layout));
      g_variant_builder_add (builder, "i", n->cell.x);
      g_variant_builder_add (builder, "i", n->cell.y);
      g_variant_builder_add (builder, "i", n->cell.width);
      g_variant_builder_add (builder, "i", n->cell.height);
      g_variant_builder_add_value (builder, g_variant_builder_end (&tabs));
      g_variant_builder_close (builder);
      return;
    }

  if (!n->children)
    return;
  for (i = 0; i < n->children->len; i++)
    collect_chrome_node (g_ptr_array_index (n->children, i), builder, focus);
}

static GVariant *
folletto_wm_get_chrome (FollettoWm *wm)
{
  GVariantBuilder builder;
  FollettoTree *tree;
  MetaWindow *focus;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(siiiia(tsb))"));
  if (!wm || wm->mode != META_WM_MODE_I3 || wm->layout_busy || wm->drag_window)
    return g_variant_builder_end (&builder);

  tree = focused_tree (wm);
  focus = meta_display_get_focus_window (wm->display);
  if (tree && tree->root)
    collect_chrome_node (tree->root, &builder, focus);
  return g_variant_builder_end (&builder);
}

META_EXPORT
GVariant *
meta_display_get_i3_chrome (MetaDisplay *display)
{
  if (!META_IS_DISPLAY (display) || !display->folletto_wm)
    {
      GVariantBuilder b;

      g_variant_builder_init (&b, G_VARIANT_TYPE ("a(siiiia(tsb))"));
      return g_variant_builder_end (&b);
    }
  return folletto_wm_get_chrome (display->folletto_wm);
}

static const char folletto_dbus_xml[] =
  "<node>"
  "  <interface name='org.folletto.Wm'>"
  "    <method name='Command'>"
  "      <arg type='s' name='command' direction='in'/>"
  "      <arg type='b' name='ok' direction='out'/>"
  "    </method>"
  "    <method name='GetLayout'>"
  "      <arg type='s' name='layout' direction='out'/>"
  "    </method>"
  "    <method name='GetMode'>"
  "      <arg type='s' name='mode' direction='out'/>"
  "    </method>"
  "    <method name='SetMode'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "    </method>"
  "    <method name='GetChrome'>"
  "      <arg type='a(siiiia(tsb))' name='chrome' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static void
folletto_dbus_method (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
  FollettoWm *wm = user_data;

  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;

  if (g_strcmp0 (method_name, "Command") == 0)
    {
      const gchar *cmd;
      gboolean ok;

      g_variant_get (parameters, "(&s)", &cmd);
      ok = folletto_wm_command (wm, cmd);
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(b)", ok));
    }
  else if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(s)",
                                                            folletto_wm_get_layout (wm)));
    }
  else if (g_strcmp0 (method_name, "GetMode") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(s)",
                                                            wm->mode == META_WM_MODE_I3
                                                            ? "i3" : "normal"));
    }
  else if (g_strcmp0 (method_name, "SetMode") == 0)
    {
      const gchar *mode;

      g_variant_get (parameters, "(&s)", &mode);
      folletto_wm_set_mode (wm, g_str_equal (mode, "i3")
                            ? META_WM_MODE_I3 : META_WM_MODE_NORMAL);
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_strcmp0 (method_name, "GetChrome") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(@a(siiiia(tsb)))",
                                                            folletto_wm_get_chrome (wm)));
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method %s", method_name);
    }
}

static const GDBusInterfaceVTable folletto_dbus_vtable = {
  folletto_dbus_method,
  NULL,
  NULL,
  { 0 }
};

static void
on_folletto_bus_acquired (GDBusConnection *connection,
                          const gchar     *name,
                          gpointer         user_data)
{
  FollettoWm *wm = user_data;
  GError *error = NULL;

  (void) name;
  wm->dbus_conn = g_object_ref (connection);
  if (!wm->dbus_info || !wm->dbus_info->interfaces)
    return;
  wm->dbus_reg_id =
    g_dbus_connection_register_object (connection,
                                       "/org/folletto/Wm",
                                       wm->dbus_info->interfaces[0],
                                       &folletto_dbus_vtable,
                                       wm, NULL, &error);
  if (error)
    {
      g_warning ("Folletto D-Bus: %s", error->message);
      g_error_free (error);
    }
}

static gboolean
folletto_wm_export_dbus_idle (gpointer data)
{
  folletto_wm_export_dbus (data);
  return G_SOURCE_REMOVE;
}

static void
folletto_wm_export_dbus (FollettoWm *wm)
{
  GError *error = NULL;

  wm->dbus_info = g_dbus_node_info_new_for_xml (folletto_dbus_xml, &error);
  if (error)
    {
      g_warning ("Folletto D-Bus xml: %s", error->message);
      g_error_free (error);
      return;
    }
  wm->dbus_owner_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    "org.folletto.Wm",
                    G_BUS_NAME_OWNER_FLAGS_REPLACE,
                    on_folletto_bus_acquired,
                    NULL, NULL,
                    wm, NULL);
}
