#pragma once

namespace jpegview_linux {

// Event ownership is resolved before dispatch. Keep the order in
// ResolveModalEventRoute aligned with the modal precedence characterized by
// the core tests; handlers remain owned by the SDL composition root.
enum class ModalEventRoute {
	ArchivePassword,
	Confirmation,
	Help,
	About,
	AdvancedConfiguration,
	FileDialog,
	BatchCopy,
	Resize,
	FreeRotation,
	PerspectiveCorrection,
	FixedCropSize,
	GoToImageNumber,
	UnsharpMask,
	PictureLevels,
	ContextMenu,
	Viewer,
};

struct ModalEventState {
	bool archivePassword = false;
	bool confirmation = false;
	bool help = false;
	bool about = false;
	bool advancedConfiguration = false;
	bool fileDialog = false;
	bool batchCopy = false;
	bool resize = false;
	bool freeRotation = false;
	bool perspectiveCorrection = false;
	bool fixedCropSize = false;
	bool goToImageNumber = false;
	bool unsharpMask = false;
	bool pictureLevels = false;
	bool contextMenu = false;
};

ModalEventRoute ResolveModalEventRoute(const ModalEventState& state);

} // namespace jpegview_linux
