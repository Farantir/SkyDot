# A container's menu: what it holds on the left, what the player carries on the right, each side the shared
# item list (ItemListView) with its own tabs, columns, search and card. Left and right (or the mouse) choose the
# side, up and down an item, page up and down the tab, accept moves the whole stack to the other side,
# `menu_drop` (R, X) takes everything the container holds, cancel, Tab or I closes. Items that are not playable
# are hidden. A Control that fills its host (GameMenu); opened by the shell when
# GameSession says `container_opened`.
class_name ContainerView
extends GameMenu

var _session: GameSession
var _container := 0
var _column := 0  # 0 the container, 1 the player
var _models: Array[ItemListModel] = []
var _lists: Array[ItemListView] = []
var _bar: BottomBar


func _init(session: GameSession) -> void:
	_session = session
	for column in 2:
		var model := ItemListModel.new("container", session.favorites)
		var list := ItemListView.new(model)
		list.region = Rect2(0.1 + 0.4 * column, 0.06, 0.4, 0.82)
		list.arrows_change_tab = false
		list.visible = false
		list.key_label = func(action: StringName) -> String: return key_label.call(action)
		list.action_requested.connect(_on_action)
		_models.append(model)
		_lists.append(list)
		add_child(list)
	_lists[1].title = "Inventory"
	_bar = BottomBar.new("inventory")
	_bar.visible = false
	add_child(_bar)
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)


## The container reference being shown (0 when closed).
func container() -> int:
	return _container if _open else 0


func open(ref: int) -> void:
	_container = ref
	var title := _session.container_name(ref)
	_lists[0].title = title if title != "" else "Container"
	_column = 0
	_models[0].tab = 0
	_models[1].tab = 0
	var was_open := _open
	_open = true
	for list in _lists:
		list.visible = true
	_bar.visible = true
	_focus(0)
	if not was_open:
		engaged.emit(true)
	_refresh()


func _closed() -> void:
	_container = 0
	for list in _lists:
		list.visible = false
		list.stop_search()
	_bar.visible = false


func current() -> Dictionary:
	return _models[_column].current()


func _refresh() -> void:
	if not _open:
		return
	var theirs := _session.inventory(_container)
	var carried := _session.inventory()
	_models[0].set_rows(ItemListSources.from_items(theirs, _session))
	_models[1].set_rows(ItemListSources.from_items(carried, _session))
	_lists[0].status = ["Weight %.1f" % ItemRows.total_weight(theirs)]
	_lists[1].status = ["Weight %.1f" % ItemRows.total_weight(carried)]
	_bar.fill_from(_session, carried)


func _menu_input(event: InputEvent) -> void:
	var handled := true
	var list := _lists[_column]
	if event is InputEventMouseButton and event.pressed:
		for column in 2:
			if Rect2(size * _lists[column].region.position, size * _lists[column].region.size).has_point(event.position):
				_focus(column)
	if list.is_searching():
		list.handle_input(event)
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"inventory"):
		close()
	elif event.is_action_pressed(&"ui_right"):
		_focus(1)
	elif event.is_action_pressed(&"ui_left"):
		_focus(0)
	elif not _lists[_column].handle_input(event):
		for other in 2:
			if other != _column and _lists[other].handle_input(event):  # the mouse, over the other side
				break
		handled = false
	_consume(event, handled)


func _focus(column: int) -> void:
	_column = column
	for i in 2:
		_lists[i].focused = i == column
		_lists[i].queue_redraw()


func _on_action(action: StringName, _row: Dictionary) -> void:
	match action:
		&"move":
			move_current()
		&"take_all":
			take_all()


## The stack under the cursor goes to the other side.
func move_current() -> void:
	var row := current()
	if row.is_empty():
		return
	var other := SkydotPapyrus.PLAYER_REF if _column == 0 else _container
	var from := _container if _column == 0 else SkydotPapyrus.PLAYER_REF
	_session.transfer(from, other, int(row["form"]), int(row["count"]))
	_refresh()


## Everything the container holds goes to the player (non-playable items too), except
## non-playable armor: a dead actor's race skin, a dragon's underskin and skin hider are what it
## wears, not loot, and taking them would strip the corpse; nameless items (effect armors) are not loot either.
func take_all() -> void:
	for row: Dictionary in _session.papyrus.inventory(_container):
		if row["name"] == "" or (not row["playable"] and row["category"] in ["armor", "clothing"]):
			continue
		_session.transfer(_container, SkydotPapyrus.PLAYER_REF, int(row["form"]), int(row["count"]), int(row["extra"]))
	_refresh()
