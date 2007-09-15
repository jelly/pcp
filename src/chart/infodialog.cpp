/*
 * Copyright (c) 2007, Aconex.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include "infodialog.h"
#include <QMessageBox>
#include "main.h"

InfoDialog::InfoDialog(QWidget* parent) : QDialog(parent)
{
    setupUi(this);
    my.pminfoStarted = false;
    my.pmvalStarted = false;
}

void InfoDialog::languageChange()
{
    retranslateUi(this);
}

void InfoDialog::reset(QString source, QString metric, QString instance,
			bool isArchive)
{
    pminfoTextEdit->setText(tr(""));
    pmvalTextEdit->setText(tr(""));
    my.pminfoStarted = false;
    my.pmvalStarted = false;
    my.isArchive = isArchive;
    my.metric = metric;
    my.source = source;
    my.instance = instance;
}

void InfoDialog::pminfo(void)
{
    my.pminfoProc = new QProcess(this);
    QStringList arguments;

    arguments << "-df";
    if (my.isArchive) {
	arguments << "-a";
	arguments << my.source;
    }
    else {
	arguments << "-h";
	arguments << my.source;
	arguments << "-tT";
    }
    arguments << my.metric;

    connect(my.pminfoProc, SIGNAL(readyReadStdout()),
	    this, SLOT(pminfoStdout()));
    connect(my.pminfoProc, SIGNAL(readyReadStderr()),
	    this, SLOT(pminfoStderr()));

    my.pminfoProc->start("pminfo", arguments);
}

void InfoDialog::pminfoStdout()
{
    QString s(my.pminfoProc->readAllStandardOutput());
    pminfoTextEdit->append(s);
}

void InfoDialog::pminfoStderr()
{
    QString s(my.pminfoProc->readAllStandardError());
    pminfoTextEdit->append(s);
}

void InfoDialog::pmval(void)
{
    QStringList arguments;
    QString port;
    port.setNum(kmtime->port());

    my.pmvalProc = new QProcess(this);
    arguments << "-f4" << "-p" << port;
    if (my.isArchive)
	arguments << "-a" << my.source;
    else
	arguments << "-h" << my.source;
    arguments << my.metric;

    connect(my.pmvalProc, SIGNAL(readyReadStdout()),
		    this, SLOT(pmvalStdout()));
    connect(my.pmvalProc, SIGNAL(readyReadStderr()),
		    this, SLOT(pmvalStderr()));
    connect(this, SIGNAL(finished(int)), this, SLOT(quit()));
    my.pmvalProc->start("pmval", arguments);
}

void InfoDialog::quit()
{
    if (my.pmvalStarted) {
	my.pmvalProc->terminate();
	my.pmvalStarted = false;
    }
    if (my.pminfoStarted) {
	my.pminfoProc->terminate();
	my.pminfoStarted = false;
    }
}

void InfoDialog::pmvalStdout()
{
    QString s(my.pmvalProc->readAllStandardOutput());
    s.trimmed();
    pmvalTextEdit->append(s);
}

void InfoDialog::pmvalStderr()
{
    QString s(my.pmvalProc->readAllStandardError());
    s.trimmed();
    s.prepend("<b>");
    s.append("</b>");
    pmvalTextEdit->append(s);
}

void InfoDialog::infoTabCurrentChanged(QWidget *current)
{
    if (current == pminfoTab) {
	if (!my.pminfoStarted) {
	    pminfo();
	    my.pminfoStarted = true;
	}
    }
    else if (current == pmvalTab) {
	if (!my.pmvalStarted) {
	    pmval();
	    my.pmvalStarted = true;
	}
    }
}
