//
//  PropertyEditors.h
//  interface/src/editor/ui
//
//  The Inspector's field widgets, one per property kind: numbers with Unity's drag-to-scrub
//  labels, vectors, colours, enums, booleans, text, JSON and the collision mask.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_PropertyEditors_h
#define hifi_PropertyEditors_h

#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QWidget>

#include "../PropertySpec.h"

class QDragEnterEvent;
class QDropEvent;
class QNetworkReply;
class QSlider;
class QTimer;

// A label that scrubs a value when dragged sideways, as Unity's numeric labels do.
class ScrubLabel : public QLabel {
    Q_OBJECT
public:
    explicit ScrubLabel(const QString& text, QWidget* parent = nullptr);
signals:
    void scrubBegan();
    void scrubbed(int deltaPixels, Qt::KeyboardModifiers modifiers);
    void scrubEnded();
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
private:
    bool _pressed { false };
    bool _scrubbing { false };
    int _lastX { 0 };
    int _pressX { 0 };
};

// A spin box that can show "—" for a mixed selection without losing its value. The wheel moves it only while it has
// the keyboard (clicked into); over any other field the wheel scrolls the panel, so nothing changes by accident. A
// run of steps, from the wheel or the arrow keys, is one edit: previewed live, done when the run pauses, Enter is
// pressed or the keyboard leaves.
class ScrubSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit ScrubSpinBox(QWidget* parent = nullptr);
    void showMixed();
    void showMark(const QString& text);   // a word in place of the value (as the mixed mark): "auto" for unset
    bool isMixedShown() const { return _mixed; }
    // something has been typed since the value was last shown
    bool isTyping() const { return lineEdit()->isModified(); }
    // a spin box sizes itself for its widest possible value, which with a huge range is absurd;
    // these keep it to a sensible field that the layout can stretch
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void stepBy(int steps) override;
signals:
    void stepRunBegan();
    void stepped();
    void stepRunEnded();
protected:
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
private:
    void endStepRun();
    bool _mixed { false };
    bool _stepping { false };
    QTimer* _stepPause { nullptr };
};

// One editable property. The label is part of it: numeric kinds get a scrubbing label.
class PropertyEditor : public QWidget {
    Q_OBJECT
public:
    explicit PropertyEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    const PropertySpec& spec() const { return _spec; }

    // the label to put beside the editor; owned by the caller's layout
    virtual QWidget* createLabel(QWidget* parent);
    // show a value; mixed when the selected entities disagree
    virtual void setValue(const QVariant& value, bool mixed) = 0;
    // the user is in the middle of it: do not refresh the value under them
    virtual bool isEditing() const { return false; }
    // Escape: typing not yet committed is dropped, the value shown before comes back
    virtual void revertTyping() {}
    // the value shown now, and whether it stands for differing values (for Copy)
    virtual QVariant currentValue() const = 0;
    virtual bool isMixed() const { return false; }
    // Copy and Paste for the row, as Unity offers on a field's label
    void showValueMenu(const QPoint& globalPos);

    static PropertyEditor* create(const PropertySpec& spec, QWidget* parent);
    // a value as clipboard text, and back; false when the text is not a value of this kind
    static QString valueToText(const PropertySpec& spec, const QVariant& value);
    static bool textToValue(const PropertySpec& spec, const QString& text, QVariant& value);

signals:
    void editBegan();                      // a scrub started: what is shown now is the undo point
    void liveValue(const QVariant& value); // while scrubbing
    void committed(const QVariant& value); // the edit is done
    void editCancelled();                  // a live edit was abandoned: back to the undo point

protected:
    PropertySpec _spec;
    bool _updating { false };              // setValue in progress: widget signals are not the user's
};

// Int, Float, Degrees; Slider, with a slider beside the field (Unity's Range): dragging it previews live and its
// release is the one undo step, as a scrub's is
class NumberEditor : public PropertyEditor {
    Q_OBJECT
public:
    // a component of a vector row gets no unit suffix: the unit is on the row's label
    NumberEditor(const PropertySpec& spec, QWidget* parent = nullptr, bool component = false);
    QWidget* createLabel(QWidget* parent) override;
    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override;
    void revertTyping() override;
    QVariant currentValue() const override { return QVariant(_spin->value()); }
    bool isMixed() const override { return _spin->isMixedShown(); }
    double value() const { return _spin->value(); }
    // connects a scrub label to this editor's value
    void attachScrubLabel(ScrubLabel* label);
private:
    void showOnSlider();
    ScrubSpinBox* _spin { nullptr };
    QSlider* _slider { nullptr };
    bool _component { false };
    bool _scrubbing { false };
    double _scrubStart { 0.0 };
    double _scrubAccumulated { 0.0 };
};

// Vec2, Vec3, Euler, Polar, Rect: a row of numbers, each with its own scrub label
class VectorEditor : public PropertyEditor {
    Q_OBJECT
public:
    VectorEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override;
    QVariant currentValue() const override;
    bool isMixed() const override;
private:
    QVector<NumberEditor*> _components;
};

class BoolEditor : public PropertyEditor {
    Q_OBJECT
public:
    BoolEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    QVariant currentValue() const override { return QVariant(_check->isChecked()); }
    bool isMixed() const override { return _check->checkState() == Qt::PartiallyChecked; }
    // the name beside the box, a Mac checkbox's, where it has no label of its own (the Inspector's header)
    void setText(const QString& text) { _check->setText(text); }
private:
    QCheckBox* _check { nullptr };
};

// String, Url
// String, Url. A link dropped on a URL field (from the Assets panel, or a browser) is taken as its value.
class TextEditor : public PropertyEditor {
    Q_OBJECT
public:
    TextEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override;
    void revertTyping() override;
    QVariant currentValue() const override { return QVariant(_edit->text()); }
    bool isMixed() const override { return _mixed; }
protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    QLineEdit* _edit { nullptr };
    QString _shown;
    bool _mixed { false };
};

// Image: a picture by URL. A thumbnail, the URL and × to clear it. A picture dragged onto it from the Assets panel, a
// browser or Finder is taken; a click on the thumbnail, or the left button held down in the field, opens a menu to
// choose a picture on this computer or clear the field. A file from this computer is uploaded first
// (uploadRequested), so that everyone sees it, and the URL it gets is what is committed.
class ImageEditor : public PropertyEditor {
    Q_OBJECT
public:
    ImageEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override;
    void revertTyping() override;
    QVariant currentValue() const override { return QVariant(_url->text()); }
    bool isMixed() const override { return _mixed; }

signals:
    void uploadRequested(const QString& localPath);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    // the thumbnail's click and the field's held press open the menu
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showThumbnail(const QString& url);
    void takeFile(const QString& localPath);
    void showMenu();
    void chooseFile();
    void clearPicture();

    QLabel* _thumbnail { nullptr };
    QLineEdit* _url { nullptr };
    QTimer* _holdTimer { nullptr };   // the left button held in the field this long opens the menu
    QPoint _holdPos;
    QPointer<QNetworkReply> _reply;
    QString _shown;
    bool _mixed { false };
};

class JsonEditor : public PropertyEditor {
    Q_OBJECT
public:
    JsonEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override;
    QVariant currentValue() const override { return QVariant(_edit->toPlainText()); }
    bool isMixed() const override { return _mixed; }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void commitIfChanged();
    QPlainTextEdit* _edit { nullptr };
    QString _shown;
    bool _mixed { false };
};

// Color, FloatColor. The picker previews live in the world; OK keeps the colour as one undo step,
// Cancel puts the old one back.
class ColorEditor : public PropertyEditor {
    Q_OBJECT
public:
    ColorEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    bool isEditing() const override { return !_dialog.isNull(); }
    QVariant currentValue() const override { return QVariant(_color); }
    bool isMixed() const override { return _mixed; }
private:
    void openPicker();
    void updateSwatch();
    QToolButton* _button { nullptr };
    QPointer<QDialog> _dialog;
    QColor _color;
    QColor _original;
    bool _mixed { false };
    bool _originalMixed { false };
};

class EnumEditor : public PropertyEditor {
    Q_OBJECT
public:
    EnumEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    QVariant currentValue() const override { return _combo->currentData(); }
    bool isMixed() const override { return _combo->currentIndex() < 0; }
private:
    QComboBox* _combo { nullptr };
};

class CollisionMaskEditor : public PropertyEditor {
    Q_OBJECT
public:
    CollisionMaskEditor(const PropertySpec& spec, QWidget* parent = nullptr);
    void setValue(const QVariant& value, bool mixed) override;
    QVariant currentValue() const override { return QVariant(currentMask()); }
    bool isMixed() const override { return _mixed; }
private:
    QString currentMask() const;
    QVector<QCheckBox*> _checks;
    bool _mixed { false };
};

#endif // hifi_PropertyEditors_h
