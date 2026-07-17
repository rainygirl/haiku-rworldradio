#ifndef HAIKU_RADIO_APP_H
#define HAIKU_RADIO_APP_H

#include <Application.h>

class App : public BApplication {
public:
	App();

	void ReadyToRun() override;
};

#endif
