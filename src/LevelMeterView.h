#ifndef HAIKU_RADIO_LEVEL_METER_VIEW_H
#define HAIKU_RADIO_LEVEL_METER_VIEW_H

#include <View.h>

// A small horizontal LED-style peak-level bar (glossy rounded segments,
// green/yellow/red by position - styled after a classic vertical studio
// LED meter, just laid on its side to fit a single status row), redrawn
// only when the level actually changes enough to matter. Level is
// 0.0-1.0; the caller (MainWindow, via a periodic BMessageRunner poll of
// RadioPlayer::CurrentLevel()) is responsible for pushing updates in -
// this view has no timer of its own.
class LevelMeterView : public BView {
public:
	explicit LevelMeterView(const char* name);

	void SetLevel(float level);

	void Draw(BRect updateRect) override;
	void GetPreferredSize(float* width, float* height) override;

private:
	float fLevel;
};

#endif
