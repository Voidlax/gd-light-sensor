#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <algorithm>

using namespace geode::prelude;

std::atomic<bool> jumping = false;
std::atomic<int> diff = 0;
std::atomic<bool> running = true;
std::atomic<bool> on = true;
std::atomic<int> threshold = 400;
bool showDiff = true;
std::string port = "COM3";
std::mutex portLock;
bool holding = false;

void loadSettings() {
	on = Mod::get()->getSettingValue<bool>("enabled");
	threshold = Mod::get()->getSettingValue<int64_t>("threshold");
	showDiff = Mod::get()->getSettingValue<bool>("show-diff");
	portLock.lock();
	port = Mod::get()->getSettingValue<std::string>("port");
	portLock.unlock();
}

int median(int* nums, int count) {
	int sorted[8];
	for (int i = 0; i < count; i++) {
		sorted[i] = nums[i];
	}
	std::sort(sorted, sorted + count);
	return sorted[count / 2];
}

void sensor() {
	while (running) {
		if (!on) {
			jumping = false;
			Sleep(200);
			continue;
		}

		portLock.lock();
		std::string name = "\\\\.\\" + port;
		portLock.unlock();

		HANDLE com = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (com == INVALID_HANDLE_VALUE) {
			jumping = false;
			Sleep(2000);
			continue;
		}

		DCB dcb = { 0 };
		dcb.DCBlength = sizeof(dcb);
		GetCommState(com, &dcb);
		dcb.BaudRate = 921600;
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fDtrControl = DTR_CONTROL_DISABLE;
		// dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_DISABLE;
		SetCommState(com, &dcb);

		COMMTIMEOUTS t = { 0 };
		t.ReadIntervalTimeout = 5;
		t.ReadTotalTimeoutConstant = 20;
		t.WriteTotalTimeoutConstant = 200;
		SetCommTimeouts(com, &t);
		PurgeComm(com, PURGE_RXCLEAR);

		DWORD n;
		WriteFile(com, "off\ngd\n", 7, &n, NULL);

		int last[8];
		int count = 0;
		int first[500];
		int firstCount = 0;
		double base = 0;
		bool down = false;
		int lo = 0;
		int hi = 0;
		ULONGLONG pressTime = 0;
		ULONGLONG stillTime = 0;
		std::string line = "";
		bool broke = false;

		while (running && on) {
			char buf[256];
			DWORD got = 0;
			if (!ReadFile(com, buf, 256, &got, NULL)) {
				broke = true;
				break;
			}

			for (int i = 0; i < (int)got; i++) {
				if (buf[i] == '\r') {
					continue;
				}
				if (buf[i] != '\n') {
					line += buf[i];
					continue;
				}
				if (line.size() == 0 || line.size() > 5 || line.find_first_not_of("0123456789") != std::string::npos) {
					line = "";
					continue;
				}
				int raw = std::stoi(line);
				line = "";

				if (count < 8) {
					last[count] = raw;
					count++;
				} else {
					for (int j = 0; j < 7; j++) {
						last[j] = last[j + 1];
					}
					last[7] = raw;
				}
				int v = median(last, count);

				if (firstCount < 500) {
					first[firstCount] = v;
					firstCount++;
					if (firstCount == 500) {
						std::sort(first, first + 500);
						base = first[250];
					}
					continue;
				}

				double d = base - v;
				diff = (int)d;
				ULONGLONG now = GetTickCount64();

				if (!down) {
					if (d > threshold) {
						down = true;
						pressTime = now;
						stillTime = now;
						lo = v;
						hi = v;
					} else if (d < threshold * 0.4 && d > -threshold * 0.4) {
						base = base + (v - base) / 2000;
					}
				} else {
					if (d < threshold * 0.5 && now - pressTime >= 40) {
						down = false;
					} else {
						if (v < lo) {
							lo = v;
						}
						if (v > hi) {
							hi = v;
						}
						if (hi - lo > 40) {
							lo = v;
							hi = v;
							stillTime = now;
						} else if (now - stillTime > 4000) {
							base = v;
							down = false;
						}
					}
				}
				jumping = down;
			}
		}

		jumping = false;
		if (!broke) {
			WriteFile(com, "x\n", 2, &n, NULL);
		}
		CloseHandle(com);
		if (broke) {
			Sleep(2000);
		}
	}
}

void letGo(GJBaseGameLayer* layer) {
	if (holding) {
		holding = false;
		layer->handleButton(false, 1, true);
	}
}

CCLabelBMFont* makeLabel() {
	auto size = CCDirector::get()->getWinSize();
	auto label = CCLabelBMFont::create("0", "bigFont.fnt");
	label->setScale(0.5f);
	label->setAnchorPoint(ccp(0, 1));
	label->setPosition(ccp(5, size.height - 5));
	label->setOpacity(150);
	label->setZOrder(100);
	label->setID("diff"_spr);
	return label;
}

$on_mod(Loaded) {
	loadSettings();
	std::thread(sensor).detach();
	std::atexit([] {
		running = false;
		Sleep(100);
	});
}

class $modify(GJBaseGameLayer) {
	void update(float dt) {
		loadSettings();
		auto pl = PlayLayer::get();
		bool want = jumping && on;
		if (pl == nullptr || static_cast<GJBaseGameLayer*>(pl) != this) {
			want = false;
		} else {
			if (pl->m_player1->m_isDead) {
				want = false;
			}
			if (CCDirector::get()->getRunningScene()->getChildByType<PauseLayer>(0)) {
				want = false;
			}
		}

		if (want != holding) {
			holding = want;
			this->handleButton(want, 1, true);
		}

		GJBaseGameLayer::update(dt);

		if (pl != nullptr && static_cast<GJBaseGameLayer*>(pl) == this) {
			auto label = static_cast<CCLabelBMFont*>(pl->getChildByID("diff"_spr));
			if (label) {
				label->setVisible(showDiff);
				label->setString(std::to_string(diff).c_str());
			}
		}
	}
};

class $modify(PlayLayer) {
	bool init(GJGameLevel* level, bool dontCreateObjects) {
		if (!PlayLayer::init(level, dontCreateObjects)) {
			return false;
		}
		this->addChild(makeLabel());
		return true;
	}

	void pauseGame(bool unfocused) {
		letGo(this);
		PlayLayer::pauseGame(unfocused);
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		letGo(this);
		PlayLayer::destroyPlayer(player, object);
	}

	void onQuit() {
		letGo(this);
		PlayLayer::onQuit();
	}
};

class $modify(MyMenu, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) {
			return false;
		}
		this->addChild(makeLabel());
		this->schedule(schedule_selector(MyMenu::updateLabel), 0.05f);
		return true;
	}

	void updateLabel(float dt) {
		loadSettings();
		auto label = static_cast<CCLabelBMFont*>(this->getChildByID("diff"_spr));
		if (label) {
			label->setVisible(showDiff);
			label->setString(std::to_string(diff).c_str());
		}
	}
};
