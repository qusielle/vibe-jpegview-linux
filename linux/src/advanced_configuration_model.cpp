#include "advanced_configuration_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jpegview_linux {
namespace {

constexpr int kMinimumVisibleRows = 1;
constexpr double kPreviewRatioStep = 0.025;
constexpr double kMagnifyingGlassZoomStep = 0.025;

constexpr const char* kTransparencyChoices[] = {"Black", "White", "Checkerboard"};

AdvancedConfigurationField BooleanField(const char* key, const char* label) {
	return {key, label, AdvancedConfigurationFieldKind::Boolean};
}

AdvancedConfigurationField ChoiceField(const char* key, const char* label,
	const char* const* choices, std::size_t choiceCount) {
	return {key, label, AdvancedConfigurationFieldKind::Choice, 0.0,
		static_cast<double>(choiceCount - 1), 1.0, 0, "", choices, choiceCount};
}

AdvancedConfigurationField IntegerField(const char* key, const char* label,
	double minimum, double maximum, double step, const char* unit = "") {
	return {key, label, AdvancedConfigurationFieldKind::Integer, minimum, maximum,
		step, 0, unit};
}

AdvancedConfigurationField DecimalField(const char* key, const char* label,
	double minimum, double maximum, double step, int precision, const char* unit = "") {
	return {key, label, AdvancedConfigurationFieldKind::Decimal, minimum, maximum,
		step, precision, unit};
}

AdvancedConfigurationField TextField(const char* key, const char* label) {
	return {key, label, AdvancedConfigurationFieldKind::Text};
}

const std::vector<AdvancedConfigurationField>& FieldsFor(int category) {
	static const std::vector<AdvancedConfigurationField> behavior = {
		BooleanField("manga_mode_inverts_left_right", "Manga mode reverses Left/Right"),
		BooleanField("spacebar_navigates_images", "Space navigates images"),
		BooleanField("folder_wrap_around", "Wrap around folder ends"),
		BooleanField("fit_relative_zoom_mode", "Fit-relative zoom mode"),
	};
	static const std::vector<AdvancedConfigurationField> appearance = {
		ChoiceField("transparency_pattern", "Transparent image background",
			kTransparencyChoices, sizeof(kTransparencyChoices) / sizeof(kTransparencyChoices[0])),
		BooleanField("show_histogram", "Show histogram"),
		TextField("window_title_pattern", "Window title pattern"),
	};
	static const std::vector<AdvancedConfigurationField> panelsAndDialogs = {
		IntegerField("thumbnail_panel_width", "Thumbnail panel width",
			kMinimumThumbnailPanelWidth, kMaximumThumbnailPanelWidth, 8.0, "px"),
		IntegerField("file_dialog_width", "File dialog width",
			kMinimumFileDialogWidth, kMaximumFileDialogDimension, 16.0, "px"),
		IntegerField("file_dialog_height", "File dialog height",
			kMinimumFileDialogHeight, kMaximumFileDialogDimension, 16.0, "px"),
		DecimalField("file_dialog_preview_ratio", "File dialog preview width",
			0.0, 0.8, kPreviewRatioStep, 3, "ratio"),
	};
	static const std::vector<AdvancedConfigurationField> magnifyingGlass = {
		IntegerField("magnifying_glass_width", "Magnifying glass width",
			MagnifyingGlassModel::kMinimumWidth, MagnifyingGlassModel::kMaximumDimension,
			30.0, "px"),
		IntegerField("magnifying_glass_height", "Magnifying glass height",
			MagnifyingGlassModel::kMinimumHeight, MagnifyingGlassModel::kMaximumDimension,
			15.0, "px"),
		DecimalField("magnifying_glass_zoom_level", "Magnifying glass zoom level",
			MagnifyingGlassModel::kMinimumZoomLevel, MagnifyingGlassModel::kMaximumZoomLevel,
			kMagnifyingGlassZoomStep, 3),
	};
	static const std::vector<AdvancedConfigurationField> crop = {
		IntegerField("user_crop_aspect_width", "User crop aspect width",
			kMinimumFixedCropDimension, kMaximumFixedCropDimension, 1.0),
		IntegerField("user_crop_aspect_height", "User crop aspect height",
			kMinimumFixedCropDimension, kMaximumFixedCropDimension, 1.0),
	};
	static const std::vector<AdvancedConfigurationField> imageDefaults = [] {
		std::vector<AdvancedConfigurationField> fields;
		fields.reserve(16);
		fields.push_back(BooleanField("default_local_density", "Default local density correction"));
		static constexpr const char* keys[] = {
			"default_contrast", "default_gamma", "default_saturation", "default_cyan_red",
			"default_magenta_green", "default_yellow_blue", "default_lighten_shadows",
			"default_darken_highlights", "default_deep_shadows", "default_color_correction",
			"default_contrast_correction", "default_sharpen",
		};
		for (std::size_t index = 0; index < static_cast<std::size_t>(LevelControl::Count); ++index) {
			const LevelControl control = static_cast<LevelControl>(index);
			const LevelControlInfo& info = GetLevelControlInfo(control);
			fields.push_back(DecimalField(keys[index], info.label, info.minimum, info.maximum,
				(info.maximum - info.minimum) / 100.0, 3));
		}
		fields.push_back(DecimalField("unsharp_mask_radius", "Unsharp mask radius",
			0.0, 5.0, 0.1, 2));
		fields.push_back(DecimalField("unsharp_mask_amount", "Unsharp mask amount",
			0.0, 10.0, 0.1, 2));
		fields.push_back(DecimalField("unsharp_mask_threshold", "Unsharp mask threshold",
			0.0, 20.0, 0.25, 2));
		return fields;
	}();
	static const std::vector<AdvancedConfigurationField> performanceAndBatch = {
		IntegerField("cache_size_mb", "Image cache budget (next launch)", 0.0,
			static_cast<double>(kMaximumCacheSizeMiB), 64.0, "MB"),
		TextField("copy_rename_pattern", "Copy/rename pattern"),
	};

	switch (category) {
	case static_cast<int>(AdvancedConfigurationCategory::Behavior): return behavior;
	case static_cast<int>(AdvancedConfigurationCategory::Appearance): return appearance;
	case static_cast<int>(AdvancedConfigurationCategory::PanelsAndDialogs): return panelsAndDialogs;
	case static_cast<int>(AdvancedConfigurationCategory::MagnifyingGlass): return magnifyingGlass;
	case static_cast<int>(AdvancedConfigurationCategory::Crop): return crop;
	case static_cast<int>(AdvancedConfigurationCategory::ImageDefaults): return imageDefaults;
	case static_cast<int>(AdvancedConfigurationCategory::PerformanceAndBatch): return performanceAndBatch;
	default: return behavior;
	}
}

constexpr const char* kCategoryNames[] = {
	"Behavior", "Appearance", "Panels & dialogs", "Magnifying glass", "Crop",
	"Image defaults", "Performance & batch",
};

struct LevelSettingKey {
	const char* key;
	LevelControl control;
};

constexpr LevelSettingKey kLevelSettingKeys[] = {
	{"default_contrast", LevelControl::Contrast},
	{"default_gamma", LevelControl::Brightness},
	{"default_saturation", LevelControl::Saturation},
	{"default_cyan_red", LevelControl::CyanRed},
	{"default_magenta_green", LevelControl::MagentaGreen},
	{"default_yellow_blue", LevelControl::YellowBlue},
	{"default_lighten_shadows", LevelControl::LightenShadows},
	{"default_darken_highlights", LevelControl::DarkenHighlights},
	{"default_deep_shadows", LevelControl::DeepShadows},
	{"default_color_correction", LevelControl::ColorCorrection},
	{"default_contrast_correction", LevelControl::ContrastCorrection},
	{"default_sharpen", LevelControl::Sharpen},
};

bool IsKey(const AdvancedConfigurationField& field, const char* key) {
	return field.key != nullptr && std::string(field.key) == key;
}

bool GetBooleanValue(const ViewerSettings& settings, const char* key) {
	if (std::string(key) == "manga_mode_inverts_left_right") return settings.mangaModeInvertsLeftRight;
	if (std::string(key) == "spacebar_navigates_images") return settings.spacebarNavigatesImages;
	if (std::string(key) == "folder_wrap_around") return settings.folderWrapAround;
	if (std::string(key) == "fit_relative_zoom_mode") return settings.fitRelativeZoomMode;
	if (std::string(key) == "show_histogram") return settings.showHistogram;
	if (std::string(key) == "default_local_density") {
		return settings.defaultImageProcessing.localDensityEnabled;
	}
	return false;
}

void SetBooleanValue(ViewerSettings& settings, const char* key, bool value) {
	if (std::string(key) == "manga_mode_inverts_left_right") settings.mangaModeInvertsLeftRight = value;
	else if (std::string(key) == "spacebar_navigates_images") settings.spacebarNavigatesImages = value;
	else if (std::string(key) == "folder_wrap_around") settings.folderWrapAround = value;
	else if (std::string(key) == "fit_relative_zoom_mode") settings.fitRelativeZoomMode = value;
	else if (std::string(key) == "show_histogram") settings.showHistogram = value;
	else if (std::string(key) == "default_local_density") {
		settings.defaultImageProcessing.localDensityEnabled = value;
	}
}

double GetNumericValue(const ViewerSettings& settings, const AdvancedConfigurationField& field) {
	if (IsKey(field, "thumbnail_panel_width")) return settings.thumbnailPanelWidth;
	if (IsKey(field, "file_dialog_width")) return settings.fileDialogWidth;
	if (IsKey(field, "file_dialog_height")) return settings.fileDialogHeight;
	if (IsKey(field, "file_dialog_preview_ratio")) return settings.fileDialogPreviewRatio;
	if (IsKey(field, "magnifying_glass_width")) return settings.magnifyingGlassWidth;
	if (IsKey(field, "magnifying_glass_height")) return settings.magnifyingGlassHeight;
	if (IsKey(field, "magnifying_glass_zoom_level")) return settings.magnifyingGlassZoomLevel;
	if (IsKey(field, "user_crop_aspect_width")) return settings.userCropAspectWidth;
	if (IsKey(field, "user_crop_aspect_height")) return settings.userCropAspectHeight;
	if (IsKey(field, "unsharp_mask_radius")) return settings.unsharpMaskRadius;
	if (IsKey(field, "unsharp_mask_amount")) return settings.unsharpMaskAmount;
	if (IsKey(field, "unsharp_mask_threshold")) return settings.unsharpMaskThreshold;
	if (IsKey(field, "cache_size_mb")) return static_cast<double>(settings.cacheSizeMiB);
	for (const LevelSettingKey& item : kLevelSettingKeys) {
		if (IsKey(field, item.key)) return GetLevelControlValue(settings.defaultImageProcessing, item.control);
	}
	return 0.0;
}

void SetNumericValue(ViewerSettings& settings, const AdvancedConfigurationField& field, double value) {
	if (field.kind == AdvancedConfigurationFieldKind::Integer) {
		value = std::clamp(value, field.minimum, field.maximum);
	}
	if (IsKey(field, "thumbnail_panel_width")) settings.thumbnailPanelWidth = static_cast<int>(std::llround(value));
	else if (IsKey(field, "file_dialog_width")) settings.fileDialogWidth = static_cast<int>(std::llround(value));
	else if (IsKey(field, "file_dialog_height")) settings.fileDialogHeight = static_cast<int>(std::llround(value));
	else if (IsKey(field, "file_dialog_preview_ratio")) settings.fileDialogPreviewRatio = value;
	else if (IsKey(field, "magnifying_glass_width")) settings.magnifyingGlassWidth = static_cast<int>(std::llround(value));
	else if (IsKey(field, "magnifying_glass_height")) settings.magnifyingGlassHeight = static_cast<int>(std::llround(value));
	else if (IsKey(field, "magnifying_glass_zoom_level")) settings.magnifyingGlassZoomLevel = value;
	else if (IsKey(field, "user_crop_aspect_width")) settings.userCropAspectWidth = static_cast<int>(std::llround(value));
	else if (IsKey(field, "user_crop_aspect_height")) settings.userCropAspectHeight = static_cast<int>(std::llround(value));
	else if (IsKey(field, "unsharp_mask_radius")) settings.unsharpMaskRadius = value;
	else if (IsKey(field, "unsharp_mask_amount")) settings.unsharpMaskAmount = value;
	else if (IsKey(field, "unsharp_mask_threshold")) settings.unsharpMaskThreshold = value;
	else if (IsKey(field, "cache_size_mb")) {
		settings.cacheSizeMiB = static_cast<std::size_t>(std::llround(value));
	} else {
		for (const LevelSettingKey& item : kLevelSettingKeys) {
			if (IsKey(field, item.key)) {
				SetLevelControlValue(settings.defaultImageProcessing, item.control, value);
				return;
			}
		}
	}
}

int ChoiceIndex(const ViewerSettings& settings, const AdvancedConfigurationField& field) {
	if (!IsKey(field, "transparency_pattern")) return 0;
	switch (settings.transparencyPattern) {
	case TransparencyPattern::Black: return 0;
	case TransparencyPattern::White: return 1;
	case TransparencyPattern::Checkerboard: return 2;
	}
	return 0;
}

void SetChoiceIndex(ViewerSettings& settings, const AdvancedConfigurationField& field, int index) {
	if (!IsKey(field, "transparency_pattern")) return;
	switch (index) {
	case 1: settings.transparencyPattern = TransparencyPattern::White; break;
	case 2: settings.transparencyPattern = TransparencyPattern::Checkerboard; break;
	default: settings.transparencyPattern = TransparencyPattern::Black; break;
	}
}

std::string FormatDecimal(double value, int precision) {
	std::ostringstream output;
	output << std::fixed << std::setprecision(std::max(0, precision)) << value;
	return output.str();
}

std::string FormatValue(const ViewerSettings& settings, const AdvancedConfigurationField& field) {
	switch (field.kind) {
	case AdvancedConfigurationFieldKind::Boolean:
		return GetBooleanValue(settings, field.key) ? "On" : "Off";
	case AdvancedConfigurationFieldKind::Choice: {
		const int index = ChoiceIndex(settings, field);
		if (field.choices == nullptr || field.choiceCount == 0) return {};
		return field.choices[std::clamp(index, 0, static_cast<int>(field.choiceCount - 1))];
	}
	case AdvancedConfigurationFieldKind::Integer:
		return std::to_string(static_cast<long long>(std::llround(GetNumericValue(settings, field))));
	case AdvancedConfigurationFieldKind::Decimal: {
		const double value = GetNumericValue(settings, field);
		if (IsKey(field, "file_dialog_preview_ratio")) {
			if (value <= 0.0) return "Auto";
			return FormatDecimal(value * 100.0, 1) + "%";
		}
		std::string result = FormatDecimal(value, field.precision);
		if (field.unit != nullptr && *field.unit != '\0') {
			if (std::string(field.unit) == "%" || std::string(field.unit) == "x") result += field.unit;
			else result += std::string(" ") + field.unit;
		}
		return result;
	}
	case AdvancedConfigurationFieldKind::Text:
		return IsKey(field, "window_title_pattern") ?
			settings.windowTitlePattern : settings.copyRenamePattern;
	}
	return {};
}

std::string FormatEditValue(const ViewerSettings& settings, const AdvancedConfigurationField& field) {
	if (field.kind == AdvancedConfigurationFieldKind::Text) {
		return IsKey(field, "window_title_pattern") ?
			settings.windowTitlePattern : settings.copyRenamePattern;
	}
	if (field.kind == AdvancedConfigurationFieldKind::Integer) {
		return std::to_string(static_cast<long long>(std::llround(GetNumericValue(settings, field))));
	}
	return FormatDecimal(GetNumericValue(settings, field), field.precision);
}

bool ParseStrictNumber(const std::string& text, bool integer, double& value) {
	if (text.empty()) return false;
	if (integer) {
		std::size_t firstDigit = 0;
		if (text[0] == '-' || text[0] == '+') firstDigit = 1;
		if (firstDigit == text.size()) return false;
		for (std::size_t index = firstDigit; index < text.size(); ++index) {
			if (text[index] < '0' || text[index] > '9') return false;
		}
		try {
			std::size_t parsedCharacters = 0;
			const long long parsed = std::stoll(text, &parsedCharacters);
			if (parsedCharacters != text.size()) return false;
			value = static_cast<double>(parsed);
			return true;
		} catch (const std::exception&) {
			return false;
		}
	}
	std::size_t cursor = 0;
	if (text[cursor] == '-' || text[cursor] == '+') ++cursor;
	bool hasMantissaDigit = false;
	while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
		hasMantissaDigit = true;
		++cursor;
	}
	if (cursor < text.size() && text[cursor] == '.') {
		++cursor;
		while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
			hasMantissaDigit = true;
			++cursor;
		}
	}
	if (!hasMantissaDigit) return false;
	if (cursor < text.size() && (text[cursor] == 'e' || text[cursor] == 'E')) {
		++cursor;
		if (cursor < text.size() && (text[cursor] == '-' || text[cursor] == '+')) ++cursor;
		const std::size_t exponentStart = cursor;
		while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
		if (cursor == exponentStart) return false;
	}
	if (cursor != text.size()) return false;
	try {
		std::size_t parsedCharacters = 0;
		const double parsed = std::stod(text, &parsedCharacters);
		if (parsedCharacters != text.size() || !std::isfinite(parsed)) return false;
		value = parsed;
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool HasLineBreakOrNul(const std::string& text) {
	return text.find('\0') != std::string::npos || text.find('\n') != std::string::npos ||
		text.find('\r') != std::string::npos;
}

void EraseLastUtf8CodePoint(std::string& text) {
	if (text.empty()) return;
	std::size_t start = text.size() - 1;
	while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0u) == 0x80u) --start;
	text.erase(start);
}

} // namespace

void AdvancedConfigurationModel::Open(const ViewerSettings& settings) {
	draft_ = settings;
	NormalizeDraft();
	activeCategory_ = static_cast<int>(AdvancedConfigurationCategory::Behavior);
	selectedRow_ = 0;
	scroll_ = 0;
	editingText_.clear();
	message_.clear();
	open_ = true;
	editing_ = false;
	inputPrimed_ = false;
	EnsureScroll();
}

void AdvancedConfigurationModel::Close() {
	open_ = false;
	editing_ = false;
	inputPrimed_ = false;
	editingText_.clear();
	message_.clear();
}

int AdvancedConfigurationModel::CategoryCount() const {
	return static_cast<int>(AdvancedConfigurationCategory::Count);
}

const char* AdvancedConfigurationModel::CategoryName(int category) const {
	if (category < 0 || category >= CategoryCount()) return "";
	return kCategoryNames[category];
}

bool AdvancedConfigurationModel::SelectCategory(int category) {
	if (category < 0 || category >= CategoryCount()) return false;
	if (category == activeCategory_) return true;
	activeCategory_ = category;
	selectedRow_ = 0;
	scroll_ = 0;
	editing_ = false;
	inputPrimed_ = false;
	editingText_.clear();
	message_.clear();
	EnsureScroll();
	return true;
}

void AdvancedConfigurationModel::MoveCategory(int direction) {
	if (direction == 0 || CategoryCount() == 0) return;
	int next = (activeCategory_ + direction % CategoryCount()) % CategoryCount();
	if (next < 0) next += CategoryCount();
	(void)SelectCategory(next);
}

int AdvancedConfigurationModel::RowCount() const {
	return static_cast<int>(FieldsFor(activeCategory_).size());
}

const AdvancedConfigurationField* AdvancedConfigurationModel::FieldAt(int row) const {
	const std::vector<AdvancedConfigurationField>& fields = FieldsFor(activeCategory_);
	if (row < 0 || row >= static_cast<int>(fields.size())) return nullptr;
	return &fields[static_cast<std::size_t>(row)];
}

int AdvancedConfigurationModel::VisibleRows() const {
	return std::max(kMinimumVisibleRows, std::min(visibleRows_, std::max(1, RowCount())));
}

void AdvancedConfigurationModel::SetVisibleRows(int rows) {
	visibleRows_ = std::max(kMinimumVisibleRows, rows);
	EnsureScroll();
}

void AdvancedConfigurationModel::MoveSelection(int direction) {
	if (direction == 0 || RowCount() <= 0) return;
	const long long moved = static_cast<long long>(selectedRow_) + direction;
	selectedRow_ = static_cast<int>(std::clamp(moved, 0ll, static_cast<long long>(RowCount() - 1)));
	editing_ = false;
	inputPrimed_ = false;
	editingText_.clear();
	message_.clear();
	EnsureSelectionVisible();
}

void AdvancedConfigurationModel::SelectRow(int row) {
	if (row < 0 || row >= RowCount()) return;
	selectedRow_ = row;
	editing_ = false;
	inputPrimed_ = false;
	editingText_.clear();
	message_.clear();
	EnsureSelectionVisible();
}

void AdvancedConfigurationModel::ScrollBy(int rows) {
	if (rows == 0) return;
	const std::size_t maximum = static_cast<std::size_t>(
		std::max(0, RowCount() - VisibleRows()));
	const long long moved = static_cast<long long>(scroll_) + rows;
	scroll_ = static_cast<std::size_t>(std::clamp(moved, 0ll,
		static_cast<long long>(maximum)));
}

const char* AdvancedConfigurationModel::FieldLabel(int row) const {
	const AdvancedConfigurationField* field = FieldAt(row);
	return field == nullptr ? "" : field->label;
}

const char* AdvancedConfigurationModel::FieldKey(int row) const {
	const AdvancedConfigurationField* field = FieldAt(row);
	return field == nullptr ? "" : field->key;
}

AdvancedConfigurationFieldKind AdvancedConfigurationModel::FieldKind(int row) const {
	const AdvancedConfigurationField* field = FieldAt(row);
	return field == nullptr ? AdvancedConfigurationFieldKind::Text : field->kind;
}

std::string AdvancedConfigurationModel::FieldValue(int row) const {
	const AdvancedConfigurationField* field = FieldAt(row);
	return field == nullptr ? std::string() : FormatValue(draft_, *field);
}

bool AdvancedConfigurationModel::ActivateSelected() {
	if (!open_ || editing_) return false;
	const AdvancedConfigurationField* field = FieldAt(selectedRow_);
	if (field == nullptr) return false;
	message_.clear();
	if (field->kind == AdvancedConfigurationFieldKind::Boolean) {
		SetBooleanValue(draft_, field->key, !GetBooleanValue(draft_, field->key));
		return true;
	}
	if (field->kind == AdvancedConfigurationFieldKind::Choice) return AdjustSelected(1);
	BeginEdit();
	return editing_;
}

bool AdvancedConfigurationModel::AdjustSelected(int direction) {
	if (!open_ || editing_ || direction == 0) return false;
	const AdvancedConfigurationField* field = FieldAt(selectedRow_);
	if (field == nullptr) return false;
	message_.clear();
	if (field->kind == AdvancedConfigurationFieldKind::Boolean) {
		SetBooleanValue(draft_, field->key, !GetBooleanValue(draft_, field->key));
		return true;
	}
	if (field->kind == AdvancedConfigurationFieldKind::Choice) {
		if (field->choiceCount == 0) return false;
		int index = ChoiceIndex(draft_, *field);
		const int count = static_cast<int>(field->choiceCount);
		index = (index + direction % count) % count;
		if (index < 0) index += count;
		SetChoiceIndex(draft_, *field, index);
		return true;
	}
	if (field->kind != AdvancedConfigurationFieldKind::Integer &&
		field->kind != AdvancedConfigurationFieldKind::Decimal) return false;
	const double current = GetNumericValue(draft_, *field);
	const long double candidate = static_cast<long double>(current) +
		static_cast<long double>(field->step) * direction;
	const double adjusted = static_cast<double>(std::clamp(candidate,
		static_cast<long double>(field->minimum), static_cast<long double>(field->maximum)));
	SetNumericValue(draft_, *field, adjusted);
	return true;
}

void AdvancedConfigurationModel::BeginEdit() {
	if (!open_ || editing_) return;
	const AdvancedConfigurationField* field = FieldAt(selectedRow_);
	if (field == nullptr || (field->kind != AdvancedConfigurationFieldKind::Integer &&
		field->kind != AdvancedConfigurationFieldKind::Decimal &&
		field->kind != AdvancedConfigurationFieldKind::Text)) return;
	editingText_ = FormatEditValue(draft_, *field);
	editing_ = true;
	inputPrimed_ = true;
	message_.clear();
}

bool AdvancedConfigurationModel::AppendText(const std::string& text) {
	if (!editing_ || text.empty()) return false;
	const AdvancedConfigurationField* field = FieldAt(selectedRow_);
	if (field == nullptr) return false;
	if (field->kind == AdvancedConfigurationFieldKind::Text && HasLineBreakOrNul(text)) {
		message_ = "Text cannot contain line breaks or NUL bytes.";
		return false;
	}
	if (field->kind == AdvancedConfigurationFieldKind::Text &&
		IsKey(*field, "window_title_pattern")) {
		const std::size_t existingBytes = inputPrimed_ ? 0 : editingText_.size();
		if (existingBytes > kMaximumWindowTitlePatternBytes ||
			text.size() > kMaximumWindowTitlePatternBytes - existingBytes) {
			message_ = "Window title pattern exceeds 1024 bytes.";
			return false;
		}
	}
	if (inputPrimed_) {
		editingText_.clear();
		inputPrimed_ = false;
	}
	editingText_ += text;
	message_.clear();
	return true;
}

void AdvancedConfigurationModel::Backspace() {
	if (!editing_) return;
	if (inputPrimed_) {
		editingText_.clear();
		inputPrimed_ = false;
	} else {
		EraseLastUtf8CodePoint(editingText_);
	}
	message_.clear();
}

void AdvancedConfigurationModel::SelectAll() {
	if (!editing_) return;
	editingText_.clear();
	inputPrimed_ = false;
	message_.clear();
}

bool AdvancedConfigurationModel::CommitEdit() {
	if (!editing_) return false;
	const AdvancedConfigurationField* field = FieldAt(selectedRow_);
	if (field == nullptr) return false;
	if (field->kind == AdvancedConfigurationFieldKind::Text) {
		if (HasLineBreakOrNul(editingText_)) {
			message_ = "Text cannot contain line breaks or NUL bytes.";
			return false;
		}
		if (IsKey(*field, "window_title_pattern")) {
			std::string error;
			const std::string normalized = NormalizeWindowTitlePattern(editingText_);
			if (!ValidateWindowTitlePattern(normalized, &error)) {
				message_ = std::move(error);
				return false;
			}
			draft_.windowTitlePattern = normalized;
		} else {
			draft_.copyRenamePattern = editingText_;
		}
		editing_ = false;
		inputPrimed_ = false;
		editingText_.clear();
		message_.clear();
		return true;
	}
	double parsed = 0.0;
	if (!ParseStrictNumber(editingText_, field->kind == AdvancedConfigurationFieldKind::Integer, parsed)) {
		message_ = field->kind == AdvancedConfigurationFieldKind::Integer ?
			"Enter a valid whole number." : "Enter a finite number.";
		return false;
	}
	const double adjusted = std::clamp(parsed, field->minimum, field->maximum);
	const bool clamped = adjusted != parsed;
	SetNumericValue(draft_, *field, adjusted);
	editing_ = false;
	inputPrimed_ = false;
	editingText_.clear();
	message_ = clamped ? "Value was clamped to the supported range." : std::string();
	return true;
}

void AdvancedConfigurationModel::CancelEdit() {
	if (!editing_) return;
	editing_ = false;
	inputPrimed_ = false;
	editingText_.clear();
	message_.clear();
}

void AdvancedConfigurationModel::SetMessage(std::string message) {
	message_ = std::move(message);
}

void AdvancedConfigurationModel::NormalizeDraft() {
	auto clampFinite = [](double value, double fallback, double minimum, double maximum) {
		return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
	};
	draft_.thumbnailPanelWidth = std::clamp(draft_.thumbnailPanelWidth,
		kMinimumThumbnailPanelWidth, kMaximumThumbnailPanelWidth);
	draft_.fileDialogWidth = std::clamp(draft_.fileDialogWidth,
		kMinimumFileDialogWidth, kMaximumFileDialogDimension);
	draft_.fileDialogHeight = std::clamp(draft_.fileDialogHeight,
		kMinimumFileDialogHeight, kMaximumFileDialogDimension);
	draft_.fileDialogPreviewRatio = clampFinite(draft_.fileDialogPreviewRatio, 0.0, 0.0, 0.8);
	draft_.magnifyingGlassWidth = std::clamp(draft_.magnifyingGlassWidth,
		MagnifyingGlassModel::kMinimumWidth, MagnifyingGlassModel::kMaximumDimension);
	draft_.magnifyingGlassHeight = std::clamp(draft_.magnifyingGlassHeight,
		MagnifyingGlassModel::kMinimumHeight, MagnifyingGlassModel::kMaximumDimension);
	draft_.magnifyingGlassZoomLevel = clampFinite(draft_.magnifyingGlassZoomLevel,
		MagnifyingGlassModel::kDefaultZoomLevel, MagnifyingGlassModel::kMinimumZoomLevel,
		MagnifyingGlassModel::kMaximumZoomLevel);
	draft_.userCropAspectWidth = std::clamp(draft_.userCropAspectWidth,
		kMinimumFixedCropDimension, kMaximumFixedCropDimension);
	draft_.userCropAspectHeight = std::clamp(draft_.userCropAspectHeight,
		kMinimumFixedCropDimension, kMaximumFixedCropDimension);
	for (std::size_t index = 0; index < static_cast<std::size_t>(LevelControl::Count); ++index) {
		const LevelControl control = static_cast<LevelControl>(index);
		SetLevelControlValue(draft_.defaultImageProcessing, control,
			GetLevelControlValue(draft_.defaultImageProcessing, control));
	}
	draft_.unsharpMaskRadius = clampFinite(draft_.unsharpMaskRadius, 1.0, 0.0, 5.0);
	draft_.unsharpMaskAmount = clampFinite(draft_.unsharpMaskAmount, 0.0, 0.0, 10.0);
	draft_.unsharpMaskThreshold = clampFinite(draft_.unsharpMaskThreshold, 4.0, 0.0, 20.0);
	draft_.cacheSizeMiB = std::min(draft_.cacheSizeMiB, kMaximumCacheSizeMiB);
	draft_.windowTitlePattern = NormalizeWindowTitlePattern(draft_.windowTitlePattern);
	if (!ValidateWindowTitlePattern(draft_.windowTitlePattern)) {
		draft_.windowTitlePattern = kDefaultWindowTitlePattern;
	}
	if (draft_.transparencyPattern != TransparencyPattern::Black &&
		draft_.transparencyPattern != TransparencyPattern::White &&
		draft_.transparencyPattern != TransparencyPattern::Checkerboard) {
		draft_.transparencyPattern = TransparencyPattern::Black;
	}
}

void AdvancedConfigurationModel::EnsureScroll() {
	const std::size_t maximum = static_cast<std::size_t>(
		std::max(0, RowCount() - VisibleRows()));
	scroll_ = std::min(scroll_, maximum);
}

void AdvancedConfigurationModel::EnsureSelectionVisible() {
	EnsureScroll();
	if (selectedRow_ < static_cast<int>(scroll_)) {
		scroll_ = static_cast<std::size_t>(selectedRow_);
	} else if (selectedRow_ >= static_cast<int>(scroll_) + VisibleRows()) {
		scroll_ = static_cast<std::size_t>(selectedRow_ - VisibleRows() + 1);
	}
	EnsureScroll();
}

} // namespace jpegview_linux
