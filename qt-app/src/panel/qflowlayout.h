#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QLayout>
#include <QStyle>
RESTORE_COMPILER_WARNINGS

class FlowLayout final : public QLayout
{
public:
	explicit FlowLayout(QWidget *parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
	explicit FlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
	~FlowLayout() override;

	void addItem(QLayoutItem *item) override;
	int horizontalSpacing() const;
	int verticalSpacing() const;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int) const override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QSize minimumSize() const override;
	void setGeometry(const QRect &rect) override;
	QSize sizeHint() const override;
	QLayoutItem *takeAt(int index) override;

private:
	int doLayout(const QRect &rect, bool testOnly) const;
	int smartSpacing(QStyle::PixelMetric pm) const;

private:
	QList<QLayoutItem *> itemList;
	int m_hSpace;
	int m_vSpace;
};
