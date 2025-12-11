
logger: main.cpp
	g++ main.cpp -O3 --std=c++20 -o logger -g -Wpedantic -fanalyzer

install:
	sudo ln -sf /home/admin/src/ctlpie/rpi/v1/logger.service /lib/systemd/system/logger.service
	sudo systemctl daemon-reload
	sudo systemctl enable logger.service
	sudo systemctl start logger.service
	sudo systemctl status logger.service

uninstall:
	sudo systemctl stop logger.service
	sudo rm /lib/systemd/system/logger.service
	sudo systemctl daemon-reload
	sudo systemctl status logger.service


clean:
	rm logger
