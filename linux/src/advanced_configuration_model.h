#pragma once

#include "settings.h"

#include <cstddef>
#include <string>

namespace jpegview_linux {

enum class AdvancedConfigurationCategory {
	Behavior,
	Appearance,
	PanelsAndDialogs,
	MagnifyingGlass,
	Crop,
	ImageDefaults,
	PerformanceAndBatch,
	Count,
};

enum class AdvancedConfigurationFieldKind {
	Boolean,
	Choice,
	Integer,
	Decimal,
	Text,
};

struct AdvancedConfigurationField {
	const char* key = "";
	const char* label = "";
	AdvancedConfigurationFieldKind kind = AdvancedConfigurationFieldKind::Text;
	double minimum = 0.0;
	double maximum = 0.0;
	double step = 0.0;
	int precision = 0;
	const char* unit = "";
	const char* const* choices = nullptr;
	std::size_t choiceCount = 0;
};

// Platform-independent draft and interaction state for the advanced settings dialog.
// Changes are made to a ViewerSettings copy; the SDL adapter decides whether to apply it.
class AdvancedConfigurationModel {
public:
	void Open(const ViewerSettings& settings);
	void Close();
	bool IsOpen() const { return open_; }
	const ViewerSettings& Draft() const { return draft_; }

	int CategoryCount() const;
	const char* CategoryName(int category) const;
	int ActiveCategory() const { return activeCategory_; }
	bool SelectCategory(int category);
	void MoveCategory(int direction);

	int RowCount() const;
	const AdvancedConfigurationField* FieldAt(int row) const;
	int SelectedRow() const { return selectedRow_; }
	std::size_t Scroll() const { return scroll_; }
	int VisibleRows() const;
	void SetVisibleRows(int rows);
	void MoveSelection(int direction);
	void SelectRow(int row);
	void ScrollBy(int rows);

	const char* FieldLabel(int row) const;
	const char* FieldKey(int row) const;
	AdvancedConfigurationFieldKind FieldKind(int row) const;
	std::string FieldValue(int row) const;
	bool ActivateSelected();
	bool AdjustSelected(int direction);

	bool IsEditing() const { return editing_; }
	const std::string& EditingText() const { return editingText_; }
	void BeginEdit();
	bool AppendText(const std::string& text);
	void Backspace();
	void SelectAll();
	bool CommitEdit();
	void CancelEdit();

	const std::string& Message() const { return message_; }
	void SetMessage(std::string message);

private:
	void NormalizeDraft();
	void EnsureScroll();
	void EnsureSelectionVisible();

	ViewerSettings draft_;
	std::string editingText_;
	std::string message_;
	std::size_t scroll_ = 0;
	int activeCategory_ = 0;
	int selectedRow_ = 0;
	int visibleRows_ = 10;
	bool open_ = false;
	bool editing_ = false;
	bool inputPrimed_ = false;
};

} // namespace jpegview_linux
