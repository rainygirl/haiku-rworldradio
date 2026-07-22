#include "LevelMeterView.h"

#include <GradientLinear.h>

LevelMeterView::LevelMeterView(const char* name)
	:
	BView(name, B_WILL_DRAW),
	fLevel(0.0f)
{
	SetExplicitMinSize(BSize(80, 18));
	SetExplicitMaxSize(BSize(80, 18));
}

void
LevelMeterView::SetLevel(float level)
{
	if (level < 0.0f)
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;
	if (level == fLevel)
		return;
	fLevel = level;
	Invalidate();
}

void
LevelMeterView::GetPreferredSize(float* width, float* height)
{
	if (width != NULL)
		*width = 80;
	if (height != NULL)
		*height = 18;
}

namespace {

const int kSegmentCount = 10;
const float kSegmentGap = 2;
const float kCornerRadius = 2;

rgb_color
Lighten(rgb_color c, float amount)
{
	return make_color(
		static_cast<uint8>(c.red + (255 - c.red) * amount),
		static_cast<uint8>(c.green + (255 - c.green) * amount),
		static_cast<uint8>(c.blue + (255 - c.blue) * amount));
}

rgb_color
Darken(rgb_color c, float amount)
{
	return make_color(
		static_cast<uint8>(c.red * (1.0f - amount)),
		static_cast<uint8>(c.green * (1.0f - amount)),
		static_cast<uint8>(c.blue * (1.0f - amount)));
}

} // namespace

void
LevelMeterView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();

	SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	FillRect(updateRect);

	int litCount = static_cast<int>(fLevel * kSegmentCount + 0.5f);
	float segmentWidth
		= (bounds.Width() - kSegmentGap * (kSegmentCount - 1)) / kSegmentCount;

	rgb_color unlitBase = Darken(ui_color(B_PANEL_BACKGROUND_COLOR), 0.25f);

	for (int i = 0; i < kSegmentCount; i++) {
		float left = bounds.left + i * (segmentWidth + kSegmentGap);
		BRect segment(left, bounds.top, left + segmentWidth, bounds.bottom);

		bool lit = i < litCount;
		rgb_color base;
		if (!lit)
			base = unlitBase;
		else if (i >= kSegmentCount - 2)
			base = make_color(220, 40, 50); // red: top two segments
		else if (i >= kSegmentCount - 4)
			base = make_color(235, 195, 30); // yellow: next two down
		else
			base = make_color(55, 190, 75); // green: everything else

		// Glossy look: lighter highlight along the top, fading to the
		// base color - same idea as the reference LED meter's polished
		// segments, just laid on its side to fit a horizontal strip.
		BGradientLinear gradient;
		gradient.SetStart(segment.left, segment.top);
		gradient.SetEnd(segment.left, segment.bottom);
		gradient.AddColor(Lighten(base, lit ? 0.55f : 0.15f), 0);
		gradient.AddColor(base, 255);
		FillRoundRect(segment, kCornerRadius, kCornerRadius, gradient);

		SetHighColor(Darken(base, 0.55f));
		StrokeRoundRect(segment, kCornerRadius, kCornerRadius);
	}
}
