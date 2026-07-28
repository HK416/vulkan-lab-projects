# Lab 01 — GPU-Driven Indirect Rendering

Study of how to issue draw commands **efficiently with `*Indirect` calls** when a
scene holds **many different meshes and materials**. The reference point is the
classic path — one CPU-recorded draw per object — measured against a GPU-driven
path where a compute shader culls and builds the draw list on the device.

## Research questions

1. **Batching** — how much does collapsing N per-object draws into one
   `vkCmdDrawIndexedIndirect(Count)` save, as N grows?
2. **Diverse geometry** — meshes of different vertex/index counts packed into
   shared buffers; each `VkDrawIndexedIndirectCommand` carries its own
   `indexCount` / `firstIndex` / `vertexOffset`. How is per-object data indexed
   (`gl_DrawID` vs `firstInstance` trick)?
3. **Diverse materials** — bindless textures via descriptor indexing so one
   pipeline draws many materials, vs pipeline/descriptor switches in the classic
   path. Cost of each approach.
4. **GPU culling** — a compute pass writes the indirect buffer + draw count
   (`vkCmdDrawIndexedIndirectCount`). Overdraw/vertex savings vs the compute
   dispatch cost.

## Method

Same scene (M instances drawn from K distinct meshes, several materials) rendered
by two backends, toggled at runtime, with GPU timestamps + frame time reported:

- **Classic** — loop over visible objects, bind/push per object, `vkCmdDrawIndexed`.
- **Indirect** — one buffer of `VkDrawIndexedIndirectCommand`, per-object data in
  an SSBO indexed by draw, single (multi)draw-indirect call; compute-culling
  variant on top.

