import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import St from 'gi://St';

import * as Main from '../main.js';
import * as PanelMenu from '../panelMenu.js';
import * as PopupMenu from '../popupMenu.js';
import * as Util from '../../misc/util.js';
import {QuickToggle, SystemIndicator} from '../quickSettings.js';

const WORKSPACE_COUNT = 9;

const LAYOUTS = [
    {id: 'grid', title: _('Griglia'), command: 'layout grid', icon: 'view-app-grid-symbolic'},
    {id: 'niri', title: _('Niri'), command: 'layout niri', icon: 'view-continuous-symbolic'},
    {id: 'splith', title: _('Tiling orizzontale'), command: 'layout splith', icon: 'view-grid-symbolic'},
    {id: 'splitv', title: _('Tiling verticale'), command: 'layout splitv', icon: 'view-dual-symbolic'},
    {id: 'tabbed', title: _('Tabbed'), command: 'layout tabbed', icon: 'tab-new-symbolic'},
    {id: 'stacked', title: _('Stacked'), command: 'layout stacked', icon: 'view-paged-symbolic'},
    {id: 'floating', title: _('Floating'), command: 'layout floating', icon: 'window-new-symbolic'},
];

const NIRI_SWIPE_FINGERS = 3;
const NIRI_SWIPE_MULTIPLIER = 2.5;
const NIRI_SCROLL_STEP = 48;
const FOLLETTO_TAB_H = 32;

const CELL_STYLE =
    'border: 2px solid rgba(255,255,255,0.35); background-color: rgba(53,132,228,0.10); border-radius: 8px;';
const CELL_ACTIVE_STYLE =
    'border: 3px solid rgb(53,132,228); background-color: rgba(53,132,228,0.32); border-radius: 8px;';

function layoutIcon(layout) {
    return LAYOUTS.find(item => item.id === layout)?.icon ?? 'view-app-grid-symbolic';
}

function currentLayout() {
    try {
        return global.display.get_i3_layout();
    } catch {
        return 'normal';
    }
}

function i3Command(command) {
    try {
        global.display.i3_command(command);
    } catch (e) {
        console.warn(`Folletto command failed: ${command}`, e);
    }
}

class FollettoDragOverlay {
    constructor() {
        this._rects = [];
        this._pollId = 0;
        this._display = global.display;
        this._layer = new St.Widget({
            name: 'folletto-drop-layer',
            reactive: false,
            visible: false,
            x: 0,
            y: 0,
        });
        Main.uiGroup.add_child(this._layer);
        this._boxes = [];
        for (let i = 0; i < 16; i++) {
            const box = new St.Widget({
                reactive: false,
                visible: false,
                style: CELL_STYLE,
            });
            this._layer.add_child(box);
            this._boxes.push(box);
        }
        this._beginId = this._display.connect('grab-op-begin', this._onBegin.bind(this));
        this._endId = this._display.connect('grab-op-end', this._onEnd.bind(this));
    }

    _onBegin(_display, window, op) {
        try {
            if (op !== Meta.GrabOp.MOVING)
                return;
            if (this._display.get_wm_mode() !== Meta.WmMode.I3)
                return;
            if (!window || window.get_window_type() !== Meta.WindowType.NORMAL)
                return;

            let tiles;
            try {
                tiles = this._display.get_i3_tiles().deepUnpack();
            } catch {
                return;
            }
            if (!tiles || tiles.length === 0)
                return;

            const valid = [];
            for (const tile of tiles) {
                if (!Array.isArray(tile) || tile.length < 4)
                    continue;
                const [x, y, w, h] = tile;
                if (![x, y, w, h].every(n => Number.isFinite(n)) || w < 8 || h < 8)
                    continue;
                if (w > 8000 || h > 8000)
                    continue;
                valid.push([x, y, w, h]);
                if (valid.length >= this._boxes.length)
                    break;
            }
            if (valid.length === 0)
                return;

            this._stopPoll();
            this._rects = valid;
            for (let i = 0; i < this._boxes.length; i++) {
                const box = this._boxes[i];
                if (i < valid.length) {
                    const [x, y, w, h] = valid[i];
                    box.set_position(x, y);
                    box.set_size(w, h);
                    box.visible = true;
                    box.set_style(CELL_STYLE);
                } else {
                    box.visible = false;
                }
            }
            this._layer.visible = true;
            this._highlightFromPointer();
            this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 32, () => {
                this._highlightFromPointer();
                return GLib.SOURCE_CONTINUE;
            });
        } catch (e) {
            console.warn('Folletto drag overlay', e);
            this._clear();
        }
    }

    _highlightFromPointer() {
        try {
            const [px, py] = global.get_pointer();
            let best = -1;
            let bestD = Infinity;
            for (let i = 0; i < this._rects.length; i++) {
                const [x, y, w, h] = this._rects[i];
                if (px >= x && px < x + w && py >= y && py < y + h) {
                    best = i;
                    break;
                }
                const dx = px - (x + w / 2);
                const dy = py - (y + h / 2);
                const d = dx * dx + dy * dy;
                if (d < bestD) {
                    bestD = d;
                    best = i;
                }
            }
            for (let i = 0; i < this._boxes.length; i++)
                this._boxes[i].set_style(i === best ? CELL_ACTIVE_STYLE : CELL_STYLE);
        } catch {
            /* pointer query can fail while the grab tears down */
        }
    }

    _onEnd() {
        this._clear();
    }

    _stopPoll() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
    }

    _clear() {
        this._stopPoll();
        this._rects = [];
        for (const box of this._boxes)
            box.visible = false;
        this._layer.visible = false;
    }
}

class FollettoTabChrome {
    constructor() {
        this._bars = [];
        this._tabButtons = [];
        this._display = global.display;
        this._idle = 0;
        this._layer = null;
        this._box = null;
        this._signature = '';
        const queue = () => this._queueSync();
        this._display.connect('wm-mode-changed', queue);
        this._display.connect('i3-layout-changed', queue);
        this._display.connect('notify::focus-window', queue);
        this._display.connect('workareas-changed', queue);
        this._display.connect('window-created', queue);
        Main.overview.connectObject(
            'showing', () => queue(),
            'hiding', () => queue(),
            this);
        /* Poll leggero: aggiorna solo se cambia la firma (niente destroy continuo). */
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
            try {
                const layout = currentLayout();
                if (layout === 'tabbed' || layout === 'stacked')
                    this._queueSync();
            } catch {
                /* ignore */
            }
            return GLib.SOURCE_CONTINUE;
        });
        this._queueSync();
    }

    _ensureLayer() {
        if (this._layer)
            return this._layer;
        this._layer = new St.Widget({
            name: 'folletto-tab-chrome',
            reactive: false,
            visible: true,
        });
        /* addChrome: sopra le finestre, sotto top_window_group (menu/popup). */
        Main.layoutManager.addChrome(this._layer, {
            affectsStruts: false,
            affectsInputRegion: true,
        });
        this._placeBelowSystemUi();
        return this._layer;
    }

    _placeBelowSystemUi() {
        const ui = Main.layoutManager.uiGroup;
        const layer = this._layer;
        if (!ui || !layer)
            return;
        try {
            const modal = Main.layoutManager.modalDialogGroup;
            if (modal && ui.contains(modal))
                ui.set_child_below_sibling(layer, modal);
        } catch {
            /* ignore */
        }
        try {
            if (global.top_window_group && ui.contains(global.top_window_group))
                ui.set_child_below_sibling(layer, global.top_window_group);
        } catch {
            /* ignore */
        }
        try {
            const panel = Main.layoutManager.panelBox;
            if (panel && ui.contains(panel))
                ui.set_child_below_sibling(layer, panel);
        } catch {
            /* ignore */
        }
    }

    _queueSync() {
        if (this._idle)
            return;
        this._idle = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._idle = 0;
            try {
                this._rebuildTabs();
            } catch (e) {
                console.warn('Folletto chrome', e);
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearBars() {
        if (this._box) {
            try {
                this._box.destroy();
            } catch {
                /* already gone */
            }
        }
        this._box = null;
        this._bars = [];
        this._tabButtons = [];
        this._signature = '';
    }

    _workspaceWindows() {
        const ws = global.workspace_manager.get_active_workspace();
        const wins = [];
        for (const actor of global.get_window_actors()) {
            let win;
            try {
                win = actor.meta_window;
            } catch {
                continue;
            }
            if (!win || win.get_window_type() !== Meta.WindowType.NORMAL)
                continue;
            if (win.minimized)
                continue;
            try {
                if (!win.located_on_workspace(ws))
                    continue;
            } catch {
                if (win.get_workspace() !== ws)
                    continue;
            }
            wins.push(win);
        }
        wins.sort((a, b) => Number(a.get_id()) - Number(b.get_id()));
        return wins;
    }

    _gapsOuter() {
        try {
            const s = new Gio.Settings({schema_id: 'org.folletto.wm'});
            return Math.max(0, s.get_int('gaps-outer'));
        } catch {
            return 4;
        }
    }

    _tabStripRect(barH) {
        const gaps = this._gapsOuter();
        const focus = this._display.focus_window;
        let monitorIndex = Main.layoutManager.primaryIndex;
        try {
            if (focus)
                monitorIndex = focus.get_monitor();
        } catch {
            /* primary */
        }
        const wa = Main.layoutManager.getWorkAreaForMonitor(monitorIndex);
        const panelBox = Main.layoutManager.panelBox;
        let top = wa.y;
        if (panelBox && panelBox.visible) {
            const panelBottom = Math.round(panelBox.y + panelBox.height);
            if (top < panelBottom)
                top = panelBottom;
        }
        return {
            x: Math.round(wa.x + gaps),
            y: Math.round(top + gaps),
            w: Math.max(Math.round(wa.width - 2 * gaps), 80),
            h: barH,
        };
    }

    _activateTab(win) {
        if (!win)
            return;
        let id = 0;
        try {
            id = win.get_id();
        } catch {
            id = 0;
        }
        if (id)
            i3Command(`focusid ${id}`);
        try {
            win.activate(global.get_current_time());
        } catch {
            /* focusid already handled WM side */
        }
        this._queueSync();
    }

    _tabStyle(focused) {
        return focused
            ? 'background-color: #3584e4; color: #ffffff; padding: 4px 10px; font-weight: bold;'
            : 'background-color: #2b2b2b; color: #deddda; padding: 4px 10px;';
    }

    _updateFocusStyles(focus) {
        for (const entry of this._tabButtons) {
            const focused = entry.win === focus;
            try {
                entry.button.set_style(this._tabStyle(focused));
                const child = entry.button.get_child();
                if (child)
                    child.set_style(focused ? 'color: #ffffff; font-weight: bold;' : 'color: #deddda;');
            } catch {
                /* ignore */
            }
        }
    }

    _rebuildTabs() {
        if (this._display.get_wm_mode() !== Meta.WmMode.I3 ||
            Main.overview.visible) {
            this._clearBars();
            return;
        }

        const layout = currentLayout();
        if (layout !== 'tabbed' && layout !== 'stacked') {
            this._clearBars();
            return;
        }

        const wins = this._workspaceWindows();
        if (wins.length === 0) {
            this._clearBars();
            return;
        }

        const focus = this._display.focus_window;
        const stacked = layout === 'stacked';
        const nTabs = Math.min(wins.length, 16);
        const slice = wins.slice(0, nTabs);
        const barH = stacked ? FOLLETTO_TAB_H * nTabs : FOLLETTO_TAB_H;
        const strip = this._tabStripRect(barH);
        const ids = slice.map(w => {
            try {
                return String(w.get_id());
            } catch {
                return '?';
            }
        }).join(',');
        const signature = `${layout}|${ids}|${strip.x},${strip.y},${strip.w},${strip.h}`;

        /* Stessa struttura: aggiorna solo highlight, non distruggere i button (sennò i click falliscono). */
        if (this._box && this._signature === signature) {
            this._updateFocusStyles(focus);
            this._placeBelowSystemUi();
            return;
        }

        this._clearBars();
        const layer = this._ensureLayer();
        const box = new St.BoxLayout({
            vertical: stacked,
            reactive: true,
            style: 'background-color: #1a1a1a; border: 1px solid #3584e4;',
        });
        box.set_position(strip.x, strip.y);
        box.set_size(strip.w, strip.h);
        layer.add_child(box);
        this._box = box;
        this._bars.push(box);
        this._signature = signature;

        for (const win of slice) {
            const focused = win === focus;
            let title = '';
            try {
                title = win.get_title() || '';
            } catch {
                title = '';
            }
            const label = title.length > 0 ? title : _('Senza titolo');
            const item = new St.Button({
                reactive: true,
                can_focus: false,
                track_hover: true,
                x_expand: !stacked,
                y_expand: stacked,
                style: this._tabStyle(focused),
            });
            item.set_child(new St.Label({
                text: label,
                y_align: Clutter.ActorAlign.CENTER,
                x_align: stacked ? Clutter.ActorAlign.START : Clutter.ActorAlign.CENTER,
                x_expand: true,
                style: focused ? 'color: #ffffff; font-weight: bold;' : 'color: #deddda;',
            }));
            item.connect('clicked', () => this._activateTab(win));
            box.add_child(item);
            this._tabButtons.push({win, button: item});
        }

        this._placeBelowSystemUi();
    }
}

class FollettoNiriGestures {
    constructor() {
        this._swipeState = 'none';
        this._display = global.display;
        this._stageId = global.stage.connect('captured-event',
            (_actor, event) => this._onEvent(event));
    }

    _active() {
        try {
            if (this._display.get_wm_mode() !== Meta.WmMode.I3)
                return false;
            return currentLayout() === 'niri';
        } catch {
            return false;
        }
    }

    _onEvent(event) {
        if (!this._active())
            return Clutter.EVENT_PROPAGATE;

        const type = event.type();

        if (type === Clutter.EventType.TOUCHPAD_SWIPE)
            return this._onTouchpadSwipe(event);

        if (type === Clutter.EventType.SCROLL)
            return this._onScroll(event);

        return Clutter.EVENT_PROPAGATE;
    }

    _onTouchpadSwipe(event) {
        try {
            if (event.get_touchpad_gesture_finger_count() !== NIRI_SWIPE_FINGERS)
                return Clutter.EVENT_PROPAGATE;

            const phase = event.get_gesture_phase();
            if (phase === Clutter.TouchpadGesturePhase.BEGIN)
                this._swipeState = 'none';

            if (this._swipeState === 'ignored')
                return Clutter.EVENT_PROPAGATE;

            const [dx, dy] = event.get_gesture_motion_delta();
            if (this._swipeState === 'none' && dx === 0 && dy === 0)
                return Clutter.EVENT_PROPAGATE;

            if (this._swipeState === 'none') {
                if (Math.abs(dx) <= Math.abs(dy)) {
                    this._swipeState = 'ignored';
                    return Clutter.EVENT_PROPAGATE;
                }
                this._swipeState = 'handling';
            }

            if (phase === Clutter.TouchpadGesturePhase.UPDATE ||
                phase === Clutter.TouchpadGesturePhase.BEGIN) {
                const delta = Math.round(-dx * NIRI_SWIPE_MULTIPLIER);
                if (delta !== 0)
                    i3Command(`scroll delta ${delta}`);
            }

            if (phase === Clutter.TouchpadGesturePhase.END ||
                phase === Clutter.TouchpadGesturePhase.CANCEL)
                this._swipeState = 'none';

            return Clutter.EVENT_STOP;
        } catch (e) {
            console.warn('Folletto niri swipe', e);
            return Clutter.EVENT_PROPAGATE;
        }
    }

    _onScroll(event) {
        try {
            let dx = 0;
            const dir = event.get_scroll_direction();
            if (dir === Clutter.ScrollDirection.SMOOTH) {
                const [sx, sy] = event.get_scroll_delta();
                /* Prefer horizontal; fall back to vertical wheel → horizontal strip. */
                if (Math.abs(sx) >= Math.abs(sy) && sx !== 0)
                    dx = sx * NIRI_SCROLL_STEP;
                else
                    dx = sy * NIRI_SCROLL_STEP;
            } else if (dir === Clutter.ScrollDirection.LEFT) {
                dx = -NIRI_SCROLL_STEP;
            } else if (dir === Clutter.ScrollDirection.RIGHT) {
                dx = NIRI_SCROLL_STEP;
            } else if (dir === Clutter.ScrollDirection.UP) {
                dx = -NIRI_SCROLL_STEP;
            } else if (dir === Clutter.ScrollDirection.DOWN) {
                dx = NIRI_SCROLL_STEP;
            }

            dx = Math.round(dx);
            if (dx === 0)
                return Clutter.EVENT_PROPAGATE;

            i3Command(`scroll delta ${dx}`);
            return Clutter.EVENT_STOP;
        } catch (e) {
            console.warn('Folletto niri scroll', e);
            return Clutter.EVENT_PROPAGATE;
        }
    }
}

class FollettoFocusBorder {
    constructor() {
        this._display = global.display;
        this._settings = null;
        try {
            this._settings = new Gio.Settings({schema_id: 'org.folletto.wm'});
        } catch (e) {
            console.warn('Folletto focus border settings', e);
        }
        this._idle = 0;
        this._border = null;
        this._focusWin = null;
        this._sizeId = 0;
        this._posId = 0;
        this._pollId = 0;
        const queue = () => this._queueSync();
        this._display.connect('wm-mode-changed', queue);
        this._display.connect('i3-layout-changed', queue);
        this._display.connect('notify::focus-window', queue);
        this._display.connect('workareas-changed', queue);
        this._display.connect('grab-op-begin', () => {
            this._startPoll();
            queue();
        });
        this._display.connect('grab-op-end', () => {
            this._stopPoll();
            queue();
        });
        Main.overview.connectObject(
            'showing', () => queue(),
            'hiding', () => queue(),
            this);
        if (this._settings) {
            this._settings.connect('changed::focus-border-enabled', queue);
            this._settings.connect('changed::focus-border-color', queue);
            this._settings.connect('changed::focus-border-width', queue);
        }
        this._queueSync();
    }

    _startPoll() {
        if (this._pollId)
            return;
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 50, () => {
            try {
                this._sync();
            } catch {
                /* ignore */
            }
            return GLib.SOURCE_CONTINUE;
        });
    }

    _stopPoll() {
        if (this._pollId) {
            GLib.source_remove(this._pollId);
            this._pollId = 0;
        }
        this._queueSync();
    }

    _trackFocusWindow(win) {
        if (this._focusWin === win)
            return;
        if (this._focusWin) {
            if (this._sizeId)
                this._focusWin.disconnect(this._sizeId);
            if (this._posId)
                this._focusWin.disconnect(this._posId);
            this._sizeId = 0;
            this._posId = 0;
            this._focusWin = null;
        }
        if (!win)
            return;
        this._focusWin = win;
        const queue = () => this._queueSync();
        this._sizeId = win.connect('size-changed', queue);
        this._posId = win.connect('position-changed', queue);
    }

    _ensureBorder() {
        if (this._border)
            return this._border;
        this._border = new St.Widget({
            name: 'folletto-focus-border',
            reactive: false,
            visible: false,
        });
        /* Chrome sotto i popup di sistema (menu), sopra le finestre. */
        Main.layoutManager.addChrome(this._border, {
            affectsStruts: false,
            affectsInputRegion: false,
        });
        try {
            const ui = Main.layoutManager.uiGroup;
            if (global.top_window_group && ui.contains(global.top_window_group))
                ui.set_child_below_sibling(this._border, global.top_window_group);
            const panel = Main.layoutManager.panelBox;
            if (panel && ui.contains(panel))
                ui.set_child_below_sibling(this._border, panel);
        } catch {
            /* ignore */
        }
        return this._border;
    }

    _queueSync() {
        if (this._idle)
            return;
        this._idle = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            this._idle = 0;
            try {
                this._sync();
            } catch (e) {
                console.warn('Folletto focus border', e);
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _settingBool(key, fallback) {
        try {
            return this._settings ? this._settings.get_boolean(key) : fallback;
        } catch {
            return fallback;
        }
    }

    _settingInt(key, fallback) {
        try {
            return this._settings ? this._settings.get_int(key) : fallback;
        } catch {
            return fallback;
        }
    }

    _settingString(key, fallback) {
        try {
            return this._settings ? this._settings.get_string(key) : fallback;
        } catch {
            return fallback;
        }
    }

    _sync() {
        const enabled = this._settingBool('focus-border-enabled', true);
        const width = this._settingInt('focus-border-width', 3);
        const border = this._ensureBorder();
        if (!enabled || width <= 0 ||
            this._display.get_wm_mode() !== Meta.WmMode.I3 ||
            Main.overview.visible) {
            this._trackFocusWindow(null);
            border.visible = false;
            return;
        }

        const focus = this._display.focus_window;
        this._trackFocusWindow(focus || null);
        if (!focus || focus.get_window_type() !== Meta.WindowType.NORMAL ||
            focus.minimized) {
            border.visible = false;
            return;
        }

        let rect;
        try {
            rect = focus.get_frame_rect();
        } catch {
            border.visible = false;
            return;
        }
        /* Finestre parcheggiate fuori schermo (tab non attive). */
        if (!rect || rect.width < 8 || rect.height < 8 ||
            rect.x < -500 || rect.y < -500) {
            border.visible = false;
            return;
        }

        const color = this._settingString('focus-border-color', '#3584e4') || '#3584e4';
        const inset = Math.max(width, 1);
        border.set_style(
            `border: ${inset}px solid ${color}; background-color: transparent;`);
        border.set_position(Math.round(rect.x), Math.round(rect.y));
        border.set_size(Math.round(rect.width), Math.round(rect.height));
        border.visible = true;
        try {
            const ui = Main.layoutManager.uiGroup;
            if (global.top_window_group && ui.contains(global.top_window_group))
                ui.set_child_below_sibling(border, global.top_window_group);
            const panel = Main.layoutManager.panelBox;
            if (panel && ui.contains(panel))
                ui.set_child_below_sibling(border, panel);
        } catch {
            /* ignore */
        }
    }
}

export const WorkspaceBar = GObject.registerClass(
class WorkspaceBar extends St.BoxLayout {
    _init() {
        super._init({
            name: 'follettoWorkspaces',
            style_class: 'panel-status-indicators-box',
            y_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
            accessible_name: _('Workspaces'),
        });

        this._overviewButton = new St.Button({
            style_class: 'panel-button',
            can_focus: true,
            track_hover: true,
            y_expand: true,
            accessible_name: _('Panoramica'),
            child: new St.Icon({
                style_class: 'system-status-icon',
                icon_name: 'view-app-grid-symbolic',
            }),
        });
        this._overviewButton.connect('clicked', () => {
            if (Main.overview.shouldToggleByCornerOrButton())
                Main.overview.toggle();
        });
        this.add_child(this._overviewButton);

        this._buttons = [];
        for (let i = 0; i < WORKSPACE_COUNT; i++) {
            const number = i + 1;
            const button = new St.Button({
                style_class: 'panel-button',
                label: `${number}`,
                can_focus: true,
                track_hover: true,
                y_expand: true,
                accessible_name: _('Workspace %d').replace('%d', `${number}`),
            });
            button.connect('clicked', () => {
                i3Command(`workspace number ${number}`);
            });
            this.add_child(button);
            this._buttons.push(button);
        }

        this._activities = null;
        this._wm = global.workspace_manager;
        this._display = global.display;
        this._wm.connectObject(
            'active-workspace-changed', () => this._sync(),
            this);
        this._display.connectObject(
            'wm-mode-changed', () => this._sync(),
            this);
        Main.sessionMode.connectObject('updated', () => this._sync(), this);
        Main.overview.connectObject(
            'showing', () => this._syncOverview(),
            'hiding', () => this._syncOverview(),
            this);
        this._sync();
    }

    trackActivities(button) {
        this._activities = button ?? null;
        this._sync();
    }

    _inUserSession() {
        return Main.sessionMode.currentMode === 'user';
    }

    _inI3() {
        try {
            return this._display.get_wm_mode() === Meta.WmMode.I3;
        } catch {
            return false;
        }
    }

    _sync() {
        const show = this._inUserSession() && this._inI3();
        this.visible = show;

        if (this._activities)
            this._activities.visible = !show;

        if (!show)
            return;

        this._syncOverview();

        const time = global.get_current_time();
        while (this._wm.n_workspaces < WORKSPACE_COUNT)
            this._wm.append_new_workspace(false, time);

        const active = this._wm.get_active_workspace_index();
        for (let i = 0; i < this._buttons.length; i++) {
            const button = this._buttons[i];
            const current = i === active;
            if (current)
                button.add_style_pseudo_class('checked');
            else
                button.remove_style_pseudo_class('checked');
        }
    }

    _syncOverview() {
        const open = Main.overview.visible;
        if (open)
            this._overviewButton.add_style_pseudo_class('checked');
        else
            this._overviewButton.remove_style_pseudo_class('checked');
    }
});

let _dragOverlay = null;
let _tabChrome = null;
let _niriGestures = null;
let _focusBorder = null;
let _chromeScheduled = false;

function ensureDragOverlay() {
    _dragOverlay ??= new FollettoDragOverlay();
}

function ensureTabChrome() {
    _tabChrome ??= new FollettoTabChrome();
}

function ensureNiriGestures() {
    _niriGestures ??= new FollettoNiriGestures();
}

function ensureFocusBorder() {
    _focusBorder ??= new FollettoFocusBorder();
}

function ensureFollettoChrome() {
    if (_chromeScheduled)
        return;
    _chromeScheduled = true;
    ensureNiriGestures();
    ensureFocusBorder();
    /* Piccolo delay: evita add_child su uiGroup durante l’init del panel. */
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 400, () => {
        try {
            ensureTabChrome();
        } catch (e) {
            console.warn('Folletto chrome init', e);
        }
        return GLib.SOURCE_REMOVE;
    });
}

const FollettoToggle = GObject.registerClass(
class FollettoToggle extends QuickToggle {
    _init() {
        super._init({
            title: _('i3 layout'),
            iconName: 'view-grid-symbolic',
        });

        this._display = global.display;
        this.connect('clicked', () => this._toggle());
        this._display.connect('wm-mode-changed', () => this._sync());
        this._sync();
    }

    _toggle() {
        const next = this._display.get_wm_mode() === Meta.WmMode.I3
            ? Meta.WmMode.NORMAL
            : Meta.WmMode.I3;
        this._display.set_wm_mode(next);
        this._sync();
        const icon = Gio.Icon.new_for_string(
            next === Meta.WmMode.I3
                ? 'view-grid-symbolic'
                : 'window-new-symbolic');
        Main.osdWindowManager.show(-1, icon,
            next === Meta.WmMode.I3 ? _('i3 mode') : _('Normal mode'));
    }

    _sync() {
        const i3 = this._display.get_wm_mode() === Meta.WmMode.I3;
        if (this.checked !== i3)
            this.set({checked: i3});
    }
});

export const Indicator = GObject.registerClass(
class Indicator extends SystemIndicator {
    _init() {
        super._init();
        ensureFollettoChrome();
        this.quickSettingsItems.push(new FollettoToggle());
    }
});

export const LayoutIndicator = GObject.registerClass(
class LayoutIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.5, _('Folletto layout'));
        ensureFollettoChrome();

        this._icon = new St.Icon({
            style_class: 'system-status-icon',
            icon_name: 'view-grid-symbolic',
        });
        this.add_child(this._icon);

        this._layoutItems = new Map();
        this._wmSettings = new Gio.Settings({schema_id: 'org.folletto.wm'});

        this._modeItem = new PopupMenu.PopupSwitchMenuItem(_('Modalità i3'), false);
        this._modeItem.connect('toggled', (item, state) => {
            global.display.set_wm_mode(state ? Meta.WmMode.I3 : Meta.WmMode.NORMAL);
        });
        this.menu.addMenuItem(this._modeItem);

        this._decoItem = new PopupMenu.PopupSwitchMenuItem(
            _('Mostra cornici'),
            this._wmSettings.get_boolean('show-decorations'));
        this._decoItem.connect('toggled', (_item, state) => {
            this._wmSettings.set_boolean('show-decorations', state);
        });
        this._wmSettings.connect('changed::show-decorations', () => {
            this._decoItem.setToggleState(
                this._wmSettings.get_boolean('show-decorations'));
        });
        this.menu.addMenuItem(this._decoItem);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem(_('Layout workspace')));

        for (const layout of LAYOUTS) {
            const item = new PopupMenu.PopupImageMenuItem(layout.title, layout.icon);
            item.connect('activate', () => {
                global.display.set_wm_mode(Meta.WmMode.I3);
                i3Command(layout.command);
                this._sync();
            });
            this.menu.addMenuItem(item);
            this._layoutItems.set(layout.id, item);
        }

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        const settings = new PopupMenu.PopupMenuItem(_('Impostazioni tiling…'));
        settings.connect('activate', () => {
            Util.spawn(['folletto-settings']);
        });
        this.menu.addMenuItem(settings);

        const shortcuts = new PopupMenu.PopupMenuItem(_('Scorciatoie da tastiera…'));
        shortcuts.connect('activate', () => {
            Util.spawn(['gnome-control-center', 'keyboard']);
        });
        this.menu.addMenuItem(shortcuts);

        this._display = global.display;
        this._display.connectObject(
            'wm-mode-changed', () => this._sync(),
            'i3-layout-changed', () => this._sync(),
            'notify::focus-window', () => this._sync(),
            this);
        this.menu.connect('open-state-changed', (menu, isOpen) => {
            if (isOpen)
                this._sync();
        });
        this._sync();
    }

    _sync() {
        const i3 = this._display.get_wm_mode() === Meta.WmMode.I3;
        const layout = currentLayout();

        this._modeItem.setToggleState(i3);
        this._icon.icon_name = i3 ? layoutIcon(layout) : 'window-new-symbolic';
        this._decoItem.setToggleState(this._wmSettings.get_boolean('show-decorations'));

        for (const [id, item] of this._layoutItems) {
            item.setOrnament(i3 && id === layout
                ? PopupMenu.Ornament.CHECK
                : PopupMenu.Ornament.NONE);
        }
    }
});
