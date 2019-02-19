#include <QtGui>
#include <QCameraInfo>
#include <QMediaMetaData>
#include <iostream>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <cstring>
#include <ctime>

#include "pos_types.h"
#include "interface.h"

Q_DECLARE_METATYPE(QCameraInfo)

AspectRatioPixmapLabel::AspectRatioPixmapLabel(QWidget *parent) : QLabel(parent)
{
	this->setMinimumSize(1, 1);
	setScaledContents(false);
}

void AspectRatioPixmapLabel::setPixmap(const QPixmap &p)
{
	pix = p;
	QLabel::setPixmap(scaledPixmap());
}

int AspectRatioPixmapLabel::heightForWidth(int width) const
{
	if(this->height())
		return pix.isNull();
	else
		return (static_cast<qreal>(pix.height()) * width) / pix.width();
}

QSize AspectRatioPixmapLabel::sizeHint() const
{
	int w = this->width();
	return QSize(w, heightForWidth(w));
}

QPixmap AspectRatioPixmapLabel::scaledPixmap() const
{
	return pix.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void AspectRatioPixmapLabel::resizeEvent(QResizeEvent *e)
{
	if(!pix.isNull())
		QLabel::setPixmap(scaledPixmap());
}

Interface::Interface(): fLogging(true), fReverse(false), fViewGoalpost(true), fPauseLog(false), fRecording(false), max_robot_num(6), log_speed(1), select_robot_num(-1), field_param(FieldParameter())
{
	qRegisterMetaType<comm_info_T>("comm_info_T");
	setAcceptDrops(true);
	log_writer.setEnable();
	positions = std::vector<PositionMarker>(max_robot_num);

	capture = new Capture;

	statusBar = new QStatusBar;
	statusBar->showMessage(QString("GameMonitor: Ready"));
	setStatusBar(statusBar);

	settings = new QSettings("./config.ini", QSettings::IniFormat);
	initializeConfig();
	logo_pos_x = field_param.field_length / 2 + field_param.field_length / 4;
	logo_pos_y = field_param.border_strip_width / 2;

	/* Run receive thread */
	const int base_udp_port = settings->value("network/port").toInt();
	for(int i = 0; i < max_robot_num; i++)
		th.push_back(new UdpServer(base_udp_port + i));

	createWindow();
	createMenus();
	connection();

	updateMapTimerId = startTimer(1000); /* timer by 1000msec */
	drawField();

	this->setWindowTitle("Humanoid League Game Monitor");
}

Interface::~Interface()
{
	if(fRecording)
		capture->stop();
}

void Interface::createMenus(void)
{
	videoMenu = menuBar()->addMenu(tr("&Cameras"));

	QList<QCameraInfo> availableCameras = capture->getCameras();
	QActionGroup *videoDevicesGroup = new QActionGroup(this);
	videoDevicesGroup->setExclusive(true);
	QAction *noVideoDeviceAction = new QAction(tr("&No Camera"), videoDevicesGroup);
	videoMenu->addAction(noVideoDeviceAction);
	videoMenu->addSeparator();
	for (const QCameraInfo &cameraInfo : availableCameras) {
		QAction *videoDeviceAction = new QAction(cameraInfo.description(), videoDevicesGroup);
		videoDeviceAction->setCheckable(true);
		videoDeviceAction->setData(QVariant::fromValue(cameraInfo));
		if (cameraInfo == QCameraInfo::defaultCamera())
			videoDeviceAction->setChecked(true);
		videoMenu->addAction(videoDeviceAction);
	}
	connect(videoDevicesGroup, SIGNAL(triggered(QAction *)), this, SLOT(updateCameraDevice(QAction *)));
}

void Interface::initializeConfig(void)
{
	const int field_w = field_param.border_strip_width * 2 + field_param.field_length;
	const int field_h = field_param.border_strip_width * 2 + field_param.field_width;
	// field figure size (pixel size of drawing area)
	settings->setValue("field_image/width" , settings->value("field_image/width", field_w));
	settings->setValue("field_image/height", settings->value("field_image/height", field_h));
	// field size is 9000x6000 millimeters (See rule book of 2018)
	// In this program, field dimensions are defined in centimeters.
	settings->setValue("field_size/x", settings->value("field_size/x", field_param.field_length * 10));
	settings->setValue("field_size/y", settings->value("field_size/y", field_param.field_width * 10));
	// marker configurations
	settings->setValue("marker/pen_size", settings->value("marker/pen_size", 3));
	settings->setValue("marker/robot_size", settings->value("marker/robot_size", 15));
	settings->setValue("marker/ball_size", settings->value("marker/ball_size", 6));
	settings->setValue("marker/goal_pole_size", settings->value("marker/goal_pole_size", 5));
	settings->setValue("marker/direction_marker_length", settings->value("marker/direction_marker_length", 20));
	settings->setValue("marker/font_size", settings->value("marker/font_size", 24));
	settings->setValue("marker/font_offset_x", settings->value("marker/font_offset_x", 8));
	settings->setValue("marker/font_offset_y", settings->value("marker/font_offset_y", 24));
	settings->setValue("marker/time_up_limit", settings->value("marker/time_up_limit", 5));
	// using UDP communication port offset
	settings->setValue("network/port", settings->value("network/port", 7110));
}

void Interface::createWindow(void)
{
	window     = new QWidget;
	reverse    = new QCheckBox("Reverse field");
	viewGoalpostCheckBox = new QCheckBox("View Goal post");
	image      = new AspectRatioPixmapLabel;
	log_step   = new QLabel;
	log_slider = new QSlider(Qt::Horizontal);
	log_slider->setRange(0, 0);
	loadLogButton = new QPushButton("Load log file");
	log1Button  = new QPushButton("x1");
	log2Button  = new QPushButton("x2");
	log5Button  = new QPushButton("x5");
	recordButton = new QPushButton("Record video");
	mainLayout  = new QGridLayout;
	checkLayout = new QHBoxLayout;
	logLayout   = new QHBoxLayout;
	logSpeedButtonLayout = new QHBoxLayout;
	labelLayout = new QGridLayout;
	for(int i = 0; i < max_robot_num; i++)
		idLayout.push_back(new QGridLayout);

	viewGoalpostCheckBox->setChecked(true);
	checkLayout->addWidget(reverse);
	checkLayout->addWidget(viewGoalpostCheckBox);
	checkLayout->addWidget(loadLogButton);
	checkLayout->addWidget(recordButton);

	logLayout->addWidget(log_step);
	logLayout->addWidget(log_slider);

	logSpeedButtonLayout->addWidget(log1Button);
	logSpeedButtonLayout->addWidget(log2Button);
	logSpeedButtonLayout->addWidget(log5Button);

	pal_state_bgcolor.setColor(QPalette::Window, QColor("#D0D0D0"));
	pal_red.   setColor(QPalette::Window, QColor("#FF8E8E"));
	pal_green. setColor(QPalette::Window, QColor("#8EFF8E"));
	pal_blue.  setColor(QPalette::Window, QColor("#8E8EFF"));
	pal_black. setColor(QPalette::Window, QColor("#000000"));
	pal_orange.setColor(QPalette::Window, QColor("#FFA540"));

	const int time_limit = settings->value("marker/time_up_limit").toInt();
	for(int i = 0; i < max_robot_num; i++) {
		robotState.push_back(new ClickWidget());
		robotState[i]->setAutoFillBackground(true);
		robotState[i]->setPalette(pal_state_bgcolor);
		robotState[i]->setFixedWidth(200);
		idLabel.push_back(new QLabel());
		idLabel[i]->setNum(i + 1);
		Robot robo;
		robo.name = new QLabel();
		robo.string = new QLabel();
		robo.cf_own = new QLabel();
		robo.cf_ball = new QLabel();
		robo.cf_own_bar = new QProgressBar();
		robo.cf_ball_bar = new QProgressBar();
		robo.time_bar = new QProgressBar();
		robo.cf_own_bar->setRange(0, 100);
		robo.cf_ball_bar->setRange(0, 100);
		robo.time_bar->setRange(0, time_limit);
		robo.time_bar->setTextVisible(false);
		robot.push_back(robo);
	}

	for(int i = 0; i < max_robot_num; i++) {
		idLayout[i]->addWidget(idLabel[i], 1, 1);
		idLayout[i]->addWidget(robot[i].name, 2, 1);
		idLayout[i]->addWidget(robot[i].string, 3, 1);
		idLayout[i]->addWidget(robot[i].cf_own_bar, 4, 1);
		idLayout[i]->addWidget(robot[i].cf_ball_bar, 5, 1);
		idLayout[i]->addWidget(robot[i].time_bar, 6, 1);
		idLayout[i]->addWidget(robot[i].cf_own, 4, 2);
		idLayout[i]->addWidget(robot[i].cf_ball, 5, 2);
		robotState[i]->setLayout(idLayout[i]);
	}

	for(int i = 0, j = 0, k = 0; i < max_robot_num; i++) {
		labelLayout->addWidget(robotState[i], k + 1, j + 1);
		if(++j == 2) { k++; j = 0; }
	}

	mainLayout->addLayout(checkLayout, 1, 1, 1, 2);
	mainLayout->addWidget(image, 2, 1);
	mainLayout->addLayout(labelLayout, 2, 2);
	mainLayout->addLayout(logLayout, 3, 1);
	mainLayout->addLayout(logSpeedButtonLayout, 3, 2);

	window->setLayout(mainLayout);
	setCentralWidget(window);
}

void Interface::drawField()
{
	origin_map = QPixmap(field_param.border_strip_width * 2 + field_param.field_length, field_param.border_strip_width * 2 + field_param.field_width);
	origin_map.fill(QColor(0, 0, 0));
	QPainter p;
	p.begin(&origin_map);
	QPen pen;
	pen.setColor(QColor(255, 255, 255));
	pen.setWidth(10);
	p.setPen(pen);
	{ // draw field lines, center circle and penalty marks
		const int field_left = field_param.border_strip_width;
		const int field_right = field_param.border_strip_width + field_param.field_length;
		const int field_top = field_param.border_strip_width;
		const int field_bottom = field_param.border_strip_width + field_param.field_width;
		p.drawLine(field_left, field_top, field_right, field_top);
		p.drawLine(field_left, field_top, field_left, field_bottom);
		p.drawLine(field_right, field_bottom, field_left, field_bottom);
		p.drawLine(field_right, field_top, field_right, field_bottom);
		const int center_line_pos = field_left + field_param.field_length / 2;
		p.drawLine(center_line_pos, field_top, center_line_pos, field_bottom);
		const int left_goal_x = field_left - field_param.goal_depth;
		const int right_goal_x = field_right + field_param.goal_depth;
		const int goal_top = field_param.field_width / 2 - field_param.goal_width / 2 + field_top;
		const int goal_bottom = goal_top + field_param.goal_width;
		p.drawLine(left_goal_x, goal_top, left_goal_x, goal_bottom);
		p.drawLine(left_goal_x, goal_top, field_left, goal_top);
		p.drawLine(left_goal_x, goal_bottom, field_left, goal_bottom);
		p.drawLine(right_goal_x, goal_top, right_goal_x, goal_bottom);
		p.drawLine(right_goal_x, goal_top, field_right, goal_top);
		p.drawLine(right_goal_x, goal_bottom, field_right, goal_bottom);
		const int left_goal_area_x = field_left + field_param.goal_area_length;
		const int right_goal_area_x = field_right - field_param.goal_area_length;
		const int goal_area_top = field_param.field_width / 2 - field_param.goal_area_width / 2 + field_top;
		const int goal_area_bottom = goal_area_top + field_param.goal_area_width;
		p.drawLine(left_goal_area_x, goal_area_top, left_goal_area_x, goal_area_bottom);
		p.drawLine(left_goal_area_x, goal_area_top, field_left, goal_area_top);
		p.drawLine(left_goal_area_x, goal_area_bottom, field_left, goal_area_bottom);
		p.drawLine(right_goal_area_x, goal_area_top, right_goal_area_x, goal_area_bottom);
		p.drawLine(right_goal_area_x, goal_area_top, field_right, goal_area_top);
		p.drawLine(right_goal_area_x, goal_area_bottom, field_right, goal_area_bottom);
		const int center_of_field_y = field_top + field_param.field_width / 2;
		const int &dia = field_param.center_circle_diameter;
		const int radius = dia / 2; // radius of center circle
		p.drawEllipse(center_line_pos - radius, center_of_field_y - radius, dia, dia);
		p.drawPoint(field_left + field_param.penalty_mark_distance, center_of_field_y);
		p.drawPoint(field_right - field_param.penalty_mark_distance, center_of_field_y);
	}
	p.end();
	map = origin_map;
	image->setPixmap(map);
}

void Interface::dragEnterEvent(QDragEnterEvent *e)
{
	if(e->mimeData()->hasFormat("text/uri-list")) {
		e->acceptProposedAction();
	}
}

void Interface::dropEvent(QDropEvent *e)
{
	filenameDrag = e->mimeData()->urls().first().toLocalFile();
}

void Interface::connection(void)
{
	connect(th[0], SIGNAL(receiveData(struct comm_info_T)), this, SLOT(decodeData1(struct comm_info_T)));
	connect(th[1], SIGNAL(receiveData(struct comm_info_T)), this, SLOT(decodeData2(struct comm_info_T)));
	connect(th[2], SIGNAL(receiveData(struct comm_info_T)), this, SLOT(decodeData3(struct comm_info_T)));
	connect(th[3], SIGNAL(receiveData(struct comm_info_T)), this, SLOT(decodeData4(struct comm_info_T)));
	connect(th[4], SIGNAL(receiveData(struct comm_info_T)), this, SLOT(decodeData5(struct comm_info_T)));
	connect(th[5], SIGNAL(receiveData(struct comm_info_T)), this, SLOT(decodeData6(struct comm_info_T)));
	connect(reverse, SIGNAL(stateChanged(int)), this, SLOT(reverseField(int)));
	connect(viewGoalpostCheckBox, SIGNAL(stateChanged(int)), this, SLOT(viewGoalpost(int)));
	connect(loadLogButton, SIGNAL(clicked()), this, SLOT(loadLogFile()));
	connect(robotState[0], SIGNAL(clicked(void)), this, SLOT(selectRobot1(void)));
	connect(robotState[1], SIGNAL(clicked(void)), this, SLOT(selectRobot2(void)));
	connect(robotState[2], SIGNAL(clicked(void)), this, SLOT(selectRobot3(void)));
	connect(robotState[3], SIGNAL(clicked(void)), this, SLOT(selectRobot4(void)));
	connect(robotState[4], SIGNAL(clicked(void)), this, SLOT(selectRobot5(void)));
	connect(robotState[5], SIGNAL(clicked(void)), this, SLOT(selectRobot6(void)));
	connect(log1Button, SIGNAL(clicked(void)), this, SLOT(logSpeed1(void)));
	connect(log2Button, SIGNAL(clicked(void)), this, SLOT(logSpeed2(void)));
	connect(log5Button, SIGNAL(clicked(void)), this, SLOT(logSpeed5(void)));
	connect(log_slider, SIGNAL(sliderPressed(void)), this, SLOT(pausePlayingLog(void)));
	connect(log_slider, SIGNAL(sliderReleased(void)), this, SLOT(changeLogPosition(void)));
	connect(recordButton, SIGNAL(clicked(void)), this, SLOT(captureButtonSlot(void)));
	connect(capture, SIGNAL(updateRecordTimeSignal(QString)), this, SLOT(showRecordTime(QString)));
	connect(capture, SIGNAL(updateRecordButtonMessage(QString)), this, SLOT(setRecordButtonText(QString)));
}

void Interface::decodeData1(struct comm_info_T comm_info)
{
	decodeUdp(comm_info, &robot[0], 0);
}

void Interface::decodeData2(struct comm_info_T comm_info)
{
	decodeUdp(comm_info, &robot[1], 1);
}

void Interface::decodeData3(struct comm_info_T comm_info)
{
	decodeUdp(comm_info, &robot[2], 2);
}

void Interface::decodeData4(struct comm_info_T comm_info)
{
	decodeUdp(comm_info, &robot[3], 3);
}

void Interface::decodeData5(struct comm_info_T comm_info)
{
	decodeUdp(comm_info, &robot[4], 4);
}

void Interface::decodeData6(struct comm_info_T comm_info)
{
	decodeUdp(comm_info, &robot[5], 5);
}

void Interface::decodeUdp(struct comm_info_T comm_info, Robot *robot_data, int num)
{
	char color_str[100];
	int color, id;

	/* MAGENTA, CYAN */
	color = (int)(comm_info.id & 0x80) >> 7;
	id    = (int)(comm_info.id & 0x7F);
	positions[num].colornum = color;

	/* record time of receive data */
	time_t timer;
	struct tm *local_time;
	timer = time(NULL);
	local_time = localtime(&timer);
	positions[num].lastReceiveTime = *local_time;

	/* ID and Color */
	sprintf(color_str, "%s %d", ((color == MAGENTA) ? "MAGENTA" : "CYAN"), id);
	robot_data->name->setText(color_str);
	/* Self-position confidence */
	robot_data->cf_own->setNum(comm_info.cf_own);
	robot_data->cf_own_bar->setValue(comm_info.cf_own);
	positions[num].self_conf = comm_info.cf_own;
	/* Ball position confidence */
	robot_data->cf_ball->setNum(comm_info.cf_ball);
	robot_data->cf_ball_bar->setValue(comm_info.cf_ball);
	const int time_limit = settings->value("marker/time_up_limit").toInt();
	robot_data->time_bar->setValue(time_limit);
	/* Role and message */
	if(strstr((const char *)comm_info.command, "Attacker")) {
		/* Red */
		robotState[num]->setPalette(pal_red);
		strcpy(positions[num].color, "red");
	} else if(strstr((const char *)comm_info.command, "Neutral")) {
		/* Green */
		robotState[num]->setPalette(pal_green);
		strcpy(positions[num].color, "green");
	} else if(strstr((const char *)comm_info.command, "Defender")) {
		/* Blue */
		robotState[num]->setPalette(pal_blue);
		strcpy(positions[num].color, "blue");
	} else if(strstr((const char *)comm_info.command, "Keeper")) {
		/* Orange */
		robotState[num]->setPalette(pal_orange);
		strcpy(positions[num].color, "orange");
	} else {
		/* Black */
		robotState[num]->setPalette(pal_state_bgcolor);
		strcpy(positions[num].color, "black");
	}
	robot_data->string->setText((char *)comm_info.command);

	positions[num].enable_pos = false;
	positions[num].enable_ball = false;
	positions[num].enable_goal_pole[0] = false;
	positions[num].enable_goal_pole[1] = false;
	int goal_pole_index = 0;
	for(int i = 0; i < MAX_COMM_INFO_OBJ; i++) {
		Object obj;
		bool exist = getCommInfoObject(comm_info.object[i], &obj);
		if(!exist) continue;
		if(obj.type == NONE) continue;
		if(obj.type == SELF_POS) {
			positions[num].pos = globalPosToImagePos(obj.pos);
			positions[num].enable_pos  = true;
		}
		if(obj.type == BALL) {
			positions[num].ball = globalPosToImagePos(obj.pos);
			positions[num].enable_ball = true;
		}
		if(obj.type == GOAL_POLE) {
			if(goal_pole_index >= 2) continue;
			positions[num].goal_pole[goal_pole_index] = globalPosToImagePos(obj.pos);
			positions[num].enable_goal_pole[goal_pole_index] = true;
			goal_pole_index++;
		}
	}
	updateMap();
	/* Voltage */
	double voltage = (comm_info.voltage << 3) / 100.0;
	log_writer.write(num + 1, color_str, (int)comm_info.fps, (double)voltage,
		(int)positions[num].pos.x, (int)positions[num].pos.y, (float)positions[num].pos.th,
		(int)positions[num].ball.x, (int)positions[num].ball.y,
		(int)positions[num].goal_pole[0].x, (int)positions[num].goal_pole[0].y,
		(int)positions[num].goal_pole[1].x, (int)positions[num].goal_pole[1].y,
		(char *)comm_info.command, (int)comm_info.cf_own, (int)comm_info.cf_ball);
}

void Interface::selectRobot1(void)
{
	selectRobot(0);
}

void Interface::selectRobot2(void)
{
	selectRobot(1);
}

void Interface::selectRobot3(void)
{
	selectRobot(2);
}

void Interface::selectRobot4(void)
{
	selectRobot(3);
}

void Interface::selectRobot5(void)
{
	selectRobot(4);
}

void Interface::selectRobot6(void)
{
	selectRobot(5);
}

void Interface::selectRobot(const int num)
{
	select_robot_num = num;
	time_t timer;
	timer = time(NULL);
	last_select_time = *localtime(&timer);
	updateMap();
}

Pos Interface::globalPosToImagePos(Pos gpos)
{
	Pos ret_pos;
	const int field_image_width = settings->value("field_image/width").toInt();
	const int field_image_height = settings->value("field_image/height").toInt();
	const int field_size_x = settings->value("field_size/x").toInt();
	const int field_size_y = settings->value("field_size/y").toInt();
	ret_pos.x =
		field_image_width - (int)((double)gpos.x * ((double)field_image_width / (double)field_size_x) + ((double)field_image_width / 2));
	ret_pos.y =
		                    (int)((double)gpos.y * ((double)field_image_height / (double)field_size_y) + ((double)field_image_height / 2));
	ret_pos.th = -gpos.th + M_PI;
	return ret_pos;
}

void Interface::setParamFromFile(std::vector<std::string> lines)
{
	for(auto line : lines) {
		LogData buf;
		QString qstr = QString(line.c_str());
		QStringList list = qstr.split(QChar(','));

		int size = list.size();
		if(size < 1) continue;
		strcpy(buf.time_str, list.at(0).toStdString().c_str());
		if(size < 2) continue;
		buf.id = list.at(1).toInt();
		if(size < 3) continue;
		strcpy(buf.color_str, list.at(2).toStdString().c_str());
		if(size < 4) continue;
		buf.fps = list.at(3).toInt();
		if(size < 5) continue;
		buf.voltage = list.at(4).toDouble();
		if(size < 6) continue;
		buf.x = list.at(5).toInt();
		if(size < 7) continue;
		buf.y = list.at(6).toInt();
		if(size < 8) continue;
		buf.theta = list.at(7).toDouble();
		if(size < 9) continue;
		buf.ball_x = list.at(8).toInt();
		if(size < 10) continue;
		buf.ball_y = list.at(9).toInt();
		if(size < 11) continue;
		buf.goal_pole_x1 = list.at(10).toInt();
		if(size < 12) continue;
		buf.goal_pole_y1 = list.at(11).toInt();
		if(size < 13) continue;
		buf.goal_pole_x2 = list.at(12).toInt();
		if(size < 14) continue;
		buf.goal_pole_y2 = list.at(13).toInt();
		if(size < 15) continue;
		buf.cf_own = list.at(14).toInt();
		if(size < 16) continue;
		buf.cf_ball = list.at(15).toInt();
		if(size < 17) continue;
		strcpy(buf.msg, list.at(16).toStdString().c_str());
		log_data.push_back(buf);
	}

	if(log_data.size() == 0) return;
	log_count = 0;
	setData(log_data[log_count]);
	QString before = QString(log_data[log_count++].time_str);
	QString after = QString(log_data[log_count].time_str);
	int interval = getInterval(before, after) / log_speed;
	QTimer::singleShot(interval, this, SLOT(updateLog()));
}

int Interface::getInterval(QString before, QString after)
{
	QStringList list_before = before.split(QChar(':'));
	QStringList list_after = after.split(QChar(':'));
	int h = list_after.at(0).toInt() - list_before.at(0).toInt();
	int m = list_after.at(1).toInt() - list_before.at(1).toInt();
	int s = list_after.at(2).toInt() - list_before.at(2).toInt();
	return ((h * 60 + m) * 60 + s) * 1000;
}

void Interface::updateLog(void)
{
	if(fPauseLog)
		return;
	setData(log_data[log_count]);
	if(log_count + 1 >= log_data.size()) return;
	QString step, str_log_count, str_log_total;
	str_log_count.setNum(log_count+1);
	str_log_total.setNum(log_data.size());
	step += str_log_count + " / " + str_log_total;
	log_slider->setValue(log_count);
	log_step->setText(step);
	QString before = QString(log_data[log_count++].time_str);
	QString after = QString(log_data[log_count].time_str);
	int interval = getInterval(before, after) / log_speed;
	QTimer::singleShot(interval, this, SLOT(updateLog()));
}

void Interface::pausePlayingLog(void)
{
	fPauseLog = true;
}

void Interface::changeLogPosition(void)
{
	fPauseLog = false;
	log_count = log_slider->value();
	updateLog();
}

void Interface::setData(LogData data)
{
	int num = data.id - 1;
	Robot *robot_data = &robot[num];

	/* ID and Color */
	robot_data->name->setText(data.color_str);
	/* Self-position confidence */
	robot_data->cf_own->setNum(data.cf_own);
	robot_data->cf_own_bar->setValue(data.cf_own);
	/* Ball position confidence */
	robot_data->cf_ball->setNum(data.cf_ball);
	robot_data->cf_ball_bar->setValue(data.cf_ball);
	/* elapsed time */
	robot_data->time_bar->setValue(0);
	/* Role and message */
	char *msg = data.msg;
	if(strstr((const char *)msg, "Attacker")) {
		/* Red */
		robotState[num]->setPalette(pal_red);
		strcpy(positions[num].color, "red");
	} else if(strstr((const char *)msg, "Neutral")) {
		/* Green */
		robotState[num]->setPalette(pal_green);
		strcpy(positions[num].color, "green");
	} else if(strstr((const char *)msg, "Defender")) {
		/* Blue */
		robotState[num]->setPalette(pal_blue);
		strcpy(positions[num].color, "blue");
	} else if(strstr((const char *)msg, "Keeper")) {
		/* Orange */
		robotState[num]->setPalette(pal_orange);
		strcpy(positions[num].color, "orange");
	} else {
		/* Black */
		robotState[num]->setPalette(pal_state_bgcolor);
		strcpy(positions[num].color, "black");
	}
	robot_data->string->setText((char *)msg);

	time_t timer;
	timer = time(NULL);
	positions[num].lastReceiveTime = *localtime(&timer);
	positions[num].enable_pos  = true;
	positions[num].enable_ball = true;
	positions[num].enable_goal_pole[0] = true;
	positions[num].enable_goal_pole[1] = true;

	positions[num].pos.x = data.x;
	positions[num].pos.y = data.y;
	positions[num].pos.th = data.theta;
	positions[num].ball.x = data.ball_x;
	positions[num].ball.y = data.ball_y;
	positions[num].goal_pole[0].x = data.goal_pole_x1;
	positions[num].goal_pole[0].y = data.goal_pole_y1;
	positions[num].goal_pole[1].x = data.goal_pole_x2;
	positions[num].goal_pole[1].y = data.goal_pole_y2;
	positions[num].self_conf = data.cf_own;

	updateMap();
}

void Interface::updateMap(void)
{
	char buf[2048];
	time_t timer;
	struct tm *local_time;
	timer = time(NULL);
	local_time = localtime(&timer);

	// Create new image for erase previous position marker
	map = origin_map;
	QPainter paint(&map);
	paint.setRenderHint(QPainter::Antialiasing);
	{ // draw team marker
		paint.setPen(QPen(QColor(0xff, 0xff, 0xff)));
		QFont font = paint.font();
		font.setPointSize(32);
		paint.setFont(font);
		paint.drawText(logo_pos_x, logo_pos_y, QString("CIT Brains"));
	}

	QFont font = paint.font();
	const int font_size = settings->value("marker/font_size").toInt();
	font.setPointSize(font_size);
	paint.setFont(font);

	const int elapsed_time = (local_time->tm_min - last_select_time.tm_min) * 60 + (local_time->tm_sec - last_select_time.tm_sec);
	if(elapsed_time >= 1) {
		select_robot_num = -1;
	}
	const int time_limit = settings->value("marker/time_up_limit").toInt();
	const int field_w = settings->value("field_image/width").toInt();
	const int field_h = settings->value("field_image/height").toInt();
	for(int i = 0; i < max_robot_num; i++) {
		if(positions[i].enable_pos) {
			int self_x = positions[i].pos.x;
			int self_y = positions[i].pos.y;
			double theta = positions[i].pos.th;
			if(fReverse) {
				self_x = field_w - self_x;
				self_y = field_h - self_y;
				theta = theta + M_PI;
			}
			const int elapsed = (local_time->tm_min - positions[i].lastReceiveTime.tm_min) * 60 + (local_time->tm_sec - positions[i].lastReceiveTime.tm_sec);
			if(elapsed > time_limit) {
				positions[i].enable_pos = false;
				positions[i].enable_ball = false;
				robotState[i]->setPalette(pal_state_bgcolor);
				robot[i].name->clear();
				robot[i].string->clear();
				robot[i].cf_own->clear();
				robot[i].cf_ball->clear();
				robot[i].cf_own_bar->setValue(0);
				robot[i].cf_ball_bar->setValue(0);
				robot[i].time_bar->setValue(0);
				continue;
			}
			// set marker color according to robot role
			QColor color = getColor(positions[i].color);
			const int robot_pen_size = settings->value("marker/pen_size").toInt();
			paint.setPen(QPen(color, robot_pen_size));
			// draw robot marker
			paint.drawPoint(self_x, self_y);
			const int robot_marker_radius = settings->value("marker/robot_size").toInt();
			paint.drawEllipse(self_x - robot_marker_radius, self_y - robot_marker_radius, robot_marker_radius * 2, robot_marker_radius * 2);
			const int robot_marker_direction_length = settings->value("marker/direction_marker_length").toInt();
			const int direction_x = self_x + robot_marker_direction_length * std::cos(theta - M_PI / 2);
			const int direction_y = self_y + robot_marker_direction_length * std::sin(theta - M_PI / 2);
			paint.drawLine(self_x, self_y, direction_x, direction_y);
			// draw robot number
			sprintf(buf, "%d", i + 1);
			const int font_offset_x = settings->value("marker/font_offset_x").toInt();
			const int font_offset_y = settings->value("marker/font_offset_y").toInt();
			paint.drawText(QPoint(self_x - font_offset_x, self_y - font_offset_y), buf);
			// draw self position confidence
			{
				constexpr int bar_width = 80;
				constexpr int bar_height = 6;
				const int bar_left = self_x - bar_width / 2;
				const int bar_top = self_y + 25;
				QPainterPath path_frame, path_conf;
				path_frame.addRect(bar_left - 2, bar_top - 2, bar_width + 4, bar_height + 4);
				paint.fillPath(path_frame, Qt::white);
				const auto conf = positions[i].self_conf;
				const int conf_width = static_cast<int>(conf / 100.0 * bar_width);
				QPen pen = paint.pen();
				constexpr int pen_size = 1;
				paint.setPen(QPen(QColor(0, 0, 0), pen_size));
				paint.setRenderHint(QPainter::NonCosmeticDefaultPen);
				paint.drawRect(bar_left, bar_top, bar_width, bar_height);
				paint.setRenderHint(QPainter::Antialiasing);
				paint.setPen(pen);
				path_conf.addRect(bar_left, bar_top, conf_width, bar_height);
				QColor color;
				if(conf < 30) {
					color = Qt::red;
				} else if(conf < 70) {
					color = QColor(0xFF, 0xA5, 0x00); // orange
				} else {
					color = Qt::green;
				}
				paint.fillPath(path_conf, color);
			}
			// highlight selected robot marker
			if(select_robot_num == i) {
				QPen pen = paint.pen();
				const int pen_size = 2;
				int circle_size;
				paint.setPen(QPen(QColor(0xff, 0x00, 0x00), pen_size));
				circle_size = 200;
				paint.drawEllipse(self_x - (circle_size / 2), self_y - (circle_size / 2), circle_size, circle_size);
				paint.setPen(QPen(QColor(0xff, 0x40, 0x40), pen_size));
				circle_size = 100;
				paint.drawEllipse(self_x - (circle_size / 2), self_y - (circle_size / 2), circle_size, circle_size);
				paint.setPen(pen);
			}
			// draw ball
			if(positions[i].enable_ball && robot[i].cf_ball->text().toInt() > 0) {
				int ball_x = positions[i].ball.x;
				int ball_y = positions[i].ball.y;
				bool flag_reverse = false;
				if((positions[i].colornum == 0 && fReverse) ||
				   (positions[i].colornum == 1 && !fReverse)) {
					flag_reverse = true;
				}
				if(flag_reverse) {
					ball_x = field_w - ball_x;
					ball_y = field_h - ball_y;
				}
				// draw ball position as orange
				const int ball_marker_size = settings->value("marker/ball_size").toInt();
				paint.setPen(QPen(QColor(0xFF, 0xA5, 0x00), ball_marker_size));
				paint.drawPoint(ball_x, ball_y);
				constexpr int ball_near_threshold = 50; // Do no draw robot number if the ball is nearby from robot.
				const int distance_ball_and_robot = std::sqrt((ball_x - self_x) * (ball_x - self_x) + (ball_y - self_y) * (ball_y - self_y));
				if(distance_ball_and_robot > ball_near_threshold) {
					sprintf(buf, "%d", i + 1);
					paint.drawText(QPoint(ball_x - font_offset_x, ball_y - font_offset_y), buf);
				}
				paint.setPen(QPen(QColor(0xFF, 0xA5, 0x00), 1));
				paint.drawLine(self_x, self_y, ball_x, ball_y);
			}
			// draw goal posts
			if(fViewGoalpost) {
				for(int j = 0; j < 2; j++) {
					if(positions[i].enable_goal_pole[j]) {
						int goal_pole_x = positions[i].goal_pole[j].x;
						int goal_pole_y = positions[i].goal_pole[j].y;
						bool flag_reverse = false;
						if((positions[i].colornum == 0 && fReverse) ||
						   (positions[i].colornum == 1 && !fReverse)) {
							flag_reverse = true;
						}
						if(flag_reverse) {
							goal_pole_x = field_w - goal_pole_x;
							goal_pole_y = field_h - goal_pole_y;
						}
						const int goal_pole_marker_size = settings->value("marker/goal_pole_size").toInt();
						paint.setPen(QPen(QColor(0xFF, 0x00, 0x00), goal_pole_marker_size));
						paint.drawPoint(goal_pole_x, goal_pole_y);
						paint.setPen(QPen(QColor(0xFF, 0x00, 0x00), 1));
						paint.drawLine(self_x, self_y, goal_pole_x, goal_pole_y);
					}
				}
			}
			robot[i].time_bar->setValue(elapsed);
		}
	}
	image->setPixmap(map);
}

QColor Interface::getColor(const char *color_name)
{
	if(!strcmp(color_name, "red")) {
		return QColor(0xFF, 0x8E, 0x8E);
	} else if(!strcmp(color_name, "black")) {
		return QColor(0x00, 0x00, 0x00);
	} else if(!strcmp(color_name, "green")) {
		return QColor(0x8E, 0xFF, 0x8E);
	} else if(!strcmp(color_name, "blue")) {
		return QColor(0x8E, 0x8E, 0xFF);
	} else if(!strcmp(color_name, "orange")) {
		return QColor(0xFF, 0xA5, 0xA0);
	} else {
		return QColor(0x00, 0x00, 0x00);
	}
}

void Interface::timerEvent(QTimerEvent *e)
{
	if(e->timerId() == updateMapTimerId) {
		updateMap();
	}
}

void Interface::reverseField(int state)
{
	if(state == Qt::Checked) {
		fReverse = true;
		logo_pos_x = field_param.field_length / 2 - field_param.field_length / 4;
	} else {
		fReverse = false;
		logo_pos_x = field_param.field_length / 2 + field_param.field_length / 4;
	}
	updateMap();
}

void Interface::viewGoalpost(int state)
{
	if(state == Qt::Checked) {
		fViewGoalpost = true;
	} else {
		fViewGoalpost = false;
	}
	updateMap();
}

void Interface::loadLogFile(void)
{
	QString fileName = QFileDialog::getOpenFileName(this, "log file", "./log", "*.log");
	std::ifstream ifs(fileName.toStdString());
	if(ifs.fail()) {
		std::cerr << "file open error" << std::endl;
		return;
	}
	std::string line;
	std::vector<std::string> lines;
	while(getline(ifs, line)) {
		lines.push_back(line);
	}
	setParamFromFile(lines);
	log_slider->setMaximum(lines.size()-1);
}

void Interface::logSpeed1(void)
{
	log_speed = 1;
}

void Interface::logSpeed2(void)
{
	log_speed = 2;
}

void Interface::logSpeed5(void)
{
	log_speed = 5;
}

void Interface::captureButtonSlot(void)
{
	if(fRecording) {
		fRecording = false;
		log_writer.stopRecord();
		capture->stop();
		setRecordButtonText(QString("Record video"));
	} else {
		fRecording = true;
		time_t timer;
		struct tm *local_time;
		char filename[1024];
		const char *video_output_path = "videos/";
		timer = time(NULL);
		local_time = localtime(&timer);
		sprintf(filename, "%s%d-%d-%d-%d-%d.mov", video_output_path, local_time->tm_year+1900, local_time->tm_mon+1, local_time->tm_mday, local_time->tm_hour, local_time->tm_min);
		capture->setFilename(QString(filename));
		log_writer.startRecord(filename);
		capture->record();
		setRecordButtonText(QString("Stop recording"));
	}
}

void Interface::updateCameraDevice(QAction *action)
{
	capture->setCamera(qvariant_cast<QCameraInfo>(action->data()));
}

void Interface::showRecordTime(QString message)
{
	statusBar->showMessage(message);
}

void Interface::setRecordButtonText(QString text)
{
	recordButton->setText(text);
}

