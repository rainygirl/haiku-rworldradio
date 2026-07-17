#include "App.h"

#include "MainWindow.h"

App::App()
	:
	BApplication("application/x-vnd.RWorldRadio-Radio")
{
}

void
App::ReadyToRun()
{
	(new MainWindow())->Show();
}
