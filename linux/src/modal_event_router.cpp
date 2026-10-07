#include "modal_event_router.h"

namespace jpegview_linux {

ModalEventRoute ResolveModalEventRoute(const ModalEventState& state) {
	if (state.archivePassword) return ModalEventRoute::ArchivePassword;
	if (state.confirmation) return ModalEventRoute::Confirmation;
	if (state.help) return ModalEventRoute::Help;
	if (state.about) return ModalEventRoute::About;
	if (state.advancedConfiguration) return ModalEventRoute::AdvancedConfiguration;
	if (state.fileDialog) return ModalEventRoute::FileDialog;
	if (state.batchCopy) return ModalEventRoute::BatchCopy;
	if (state.resize) return ModalEventRoute::Resize;
	if (state.freeRotation) return ModalEventRoute::FreeRotation;
	if (state.fixedCropSize) return ModalEventRoute::FixedCropSize;
	if (state.goToImageNumber) return ModalEventRoute::GoToImageNumber;
	if (state.unsharpMask) return ModalEventRoute::UnsharpMask;
	if (state.pictureLevels) return ModalEventRoute::PictureLevels;
	if (state.contextMenu) return ModalEventRoute::ContextMenu;
	return ModalEventRoute::Viewer;
}

} // namespace jpegview_linux
