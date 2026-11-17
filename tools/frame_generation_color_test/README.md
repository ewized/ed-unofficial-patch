# Frame-generation color adapter rig

This isolated rig links `src/d3d11/frame_generation_color.cpp` directly. It
does not duplicate the conversion shader or touch Elite, SteamVR, NGX,
installed files, or graphics settings.

`run.cmd` builds with native MSVC `/W4 /WX /O2` and runs the CPU checks by
default. D3D11 and the compiler use delay imports; the CPU checks assert
neither DLL loaded, exercise OFF and owner refusals with poison COM pointers,
and test the production format/descriptor admission helpers. `--self-test`
selects the same CPU path.

`--gpu` additionally opts into a separate WARP FL11 device. It calls the
production adapter for all seven accepted formats, checks every source and
output byte (including non-square dispatch edges and HDR F16 bits), output
reuse and resizing, both eyes, exact CS shader/class/SRV/UAV restoration,
untouched CB/sampler/PS bindings, foreign owner/context/device and array
refusals, scratch alias refusals, and authoritative empty-UI identity planes.
Dynamic linkage is a required GPU fixture. D3D11 debug errors fail the test
when the optional SDK debug layer is available.

The WARP path is a correctness test, not a GPU performance measurement or a
proof that NGX generates or SteamVR presents frames. The default CPU run does
not qualify GPU behavior. Full repository validation and GPU execution are
owned by the integration controller; this directory does not modify the
repository build gates.
