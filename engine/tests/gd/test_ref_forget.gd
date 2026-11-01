extends "gd_test.gd"
# Every per-reference store of SkydotPapyrus is in the forget registry: state set on a reference is gone once
# forget_ref ran, and every store is back to what it held before. Works on the unconfigured fake (no pack): the
# stores that need the world are covered by the sse test sse_ref_forget.
const Fakes := preload("fakes.gd")

const REF := 0xFF001234

# The stores SkydotPapyrus itself registers (vm/papyrus_runtime.cpp register_forgets) and the VM's own.
const STORES := ["vm.instances", "vm.inventory", "vm.animation_states", "vm.scheduled", "placed", "destruction",
	"ref_states", "actor_states", "item_states", "attached", "loaded", "volume_loaded", "ghost_reported", "triggers",
	"harvests", "last_damager", "pending_activations", "dialogue_talked", "scene_pushed_packages",
	"quest_alias_fills", "actor_placement"]


func test_every_store_is_registered() -> void:
	var sizes := Fakes.papyrus().forget_store_sizes()
	for store: String in STORES:
		expect(sizes.has(store), "the store %s is registered" % store)


func test_forgetting_a_reference_restores_every_store() -> void:
	var papyrus := Fakes.papyrus()
	var before := papyrus.forget_store_sizes()
	papyrus.set_locked(REF, true)
	papyrus.set_open_state(REF, 3)
	papyrus.set_animation_state(REF, PackedByteArray([1, 2, 3]))
	var during := papyrus.forget_store_sizes()
	expect(during != before, "state set on a reference shows in the stores: %s" % [during])
	papyrus.forget_ref(REF)
	expect_eq(papyrus.forget_store_sizes(), before, "forget_ref restores every store")
