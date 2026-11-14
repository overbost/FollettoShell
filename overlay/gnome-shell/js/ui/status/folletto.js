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

const LAYOUTS = [
    {id: 'splith', title: _('Tiling orizzontale'), command: 'layout splith', icon: 'view-grid-symbolic'},
    {id: 'splitv', title: _('Tiling verticale'), command: 'layout splitv', icon: 'view-dual-symbolic'},
    {id: 'tabbed', title: _('Tabbed'), command: 'layout tabbed', icon: 'tab-new-symbolic'},
    {id: 'stacked', title: _('Stacked'), command: 'layout stacked', icon: 'view-paged-symbolic'},
    {id: 'floating', title: _('Floating'), command: 'layout floating', icon: 'window-new-symbolic'},
];

const CELL_STYLE =
    'border: 2px solid rgba(255,255,255,0.35); background-color: rgba(53,132,228,0.10); border-radius: 8px;';
const CELL_ACTIVE_STYLE =
    'border: 3px solid rgb(53,132,228); background-color: rgba(53,132,228,0.32); border-radius: 8px;';

function layoutIcon(layout) {
    return LAYOUTS.find(item => item.id === layout)?.icon ?? 'view-grid-symbolic';
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
        this._display = global.display;
        this._idle = 0;
        const queue = () => this._queueSync();
        this._display.connect('wm-mode-changed', queue);
        this._display.connect('i3-layout-changed', queue);
        this._display.connect('notify::focus-window', queue);
        this._queueSync();
    }

    _queueSync() {
        if (this._idle)
            return;
        this._idle = GLib.idle_add(GLib.PRIORITY_LOW, () => {
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
        for (const bar of this._bars) {
            try {
                bar.visible = false;
                bar.destroy();
            } catch {
                /* already gone */
            }
        }
        this._bars = [];
    }

    _rebuildTabs() {
        if (this._display.get_wm_mode() !== Meta.WmMode.I3) {
            this._clearBars();
            return;
        }

        let groups;
        try {
            groups = this._display.get_i3_chrome().deepUnpack();
        } catch {
            return;
        }
        if (!groups || groups.length === 0) {
            const layout = currentLayout();
            if (layout === 'tabbed' || layout === 'stacked')
                return;
            this._clearBars();
            return;
        }
        this._clearBars();

        const panelBox = Main.layoutManager.panelBox;
        if (!panelBox)
            return;

        for (const group of groups) {
            if (!Array.isArray(group) || group.length < 6)
                continue;
            const [layout, x, _y, w, _h, tabs] = group;
            if (!tabs || tabs.length === 0)
                continue;
            if (![x, w].every(n => Number.isFinite(n)) || w < 8)
                continue;

            const stacked = layout === 'stacked';
            const nTabs = Math.min(tabs.length, 16);
            const box = new St.BoxLayout({
                vertical: false,
                reactive: true,
                x_expand: false,
                y_expand: false,
                style: stacked
                    ? 'background-color: #1e1e1e; border-left: 3px solid #3584e4;'
                    : 'background-color: #1e1e1e;',
            });
            /* Stay inside the panel allocation so we never cover window actors. */
            box.set_position(Math.round(x), 0);
            box.set_size(Math.round(Math.min(w, panelBox.width)), panelBox.height);
            panelBox.add_child(box);
            this._bars.push(box);
            this._fillBar(box, false, tabs.slice(0, nTabs));
        }
    }

    _fillBar(box, stacked, tabs) {
        for (const tab of tabs) {
            if (!Array.isArray(tab) || tab.length < 3)
                continue;
            const [id, title, focused] = tab;
            const label = title && String(title).length > 0 ? String(title) : _('Senza titolo');
            const item = new St.Button({
                reactive: true,
                can_focus: false,
                x_expand: !stacked,
                y_expand: stacked,
                style: focused
                    ? 'background-color: #3584e4; color: #ffffff; padding: 4px 10px; font-weight: bold;'
                    : 'background-color: #242424; color: #deddda; padding: 4px 10px;',
            });
            item.set_child(new St.Label({
                text: label,
                y_align: Clutter.ActorAlign.CENTER,
                x_align: stacked ? Clutter.ActorAlign.START : Clutter.ActorAlign.CENTER,
                x_expand: true,
                style: focused ? 'color: #ffffff; font-weight: bold;' : 'color: #deddda;',
            }));
            if (id)
                item.connect('clicked', () => i3Command(`focusid ${id}`));
            box.add_child(item);
        }
    }
}

let _dragOverlay = null;
let _tabChrome = null;

function ensureDragOverlay() {
    _dragOverlay ??= new FollettoDragOverlay();
}

function ensureTabChrome() {
    _tabChrome ??= new FollettoTabChrome();
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
        this.quickSettingsItems.push(new FollettoToggle());
    }
});

export const LayoutIndicator = GObject.registerClass(
class LayoutIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.5, _('Folletto layout'));
        /* Drop overlay on uiGroup crashes nested Mutter during layout_tree. */

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
