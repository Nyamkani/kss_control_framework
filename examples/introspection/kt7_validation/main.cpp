#include "kcf_tool/backend/kcf_backend.hpp"
#include "kcf_tool/ui/main_window.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTreeWidget>
#include <cassert>
#include <csignal>
#include <iostream>

int main(int argc,char** argv){
    QApplication app(argc,argv);assert(argc==2);
    const QString prefix="/kt7_"+QString::number(QCoreApplication::applicationPid());
    const QString nameA=prefix+"_A",nameB=prefix+"_B";
    QProcess a,b,standalone;
    a.setProcessChannelMode(QProcess::MergedChannels);b.setProcessChannelMode(QProcess::MergedChannels);standalone.setProcessChannelMode(QProcess::MergedChannels);
    a.start(argv[1],{"--application",nameA,prefix+"/a"});assert(a.waitForStarted());
    b.start(argv[1],{"--application",nameB,prefix+"/b",prefix+"/c"});assert(b.waitForStarted());
    standalone.start(argv[1],{"--element",prefix+"/d"});assert(standalone.waitForStarted());
    kcf_tool::MainWindow window(std::make_unique<kcf_tool::KcfBackend>(),"KCF");window.show();
    auto* refresh=window.findChild<QPushButton*>("refreshButton");
    auto* tree=window.findChild<QTreeWidget*>("applicationsTree");
    auto* endpoints=window.findChild<QTreeWidget*>("elementEndpoints");
    auto* elements=window.findChild<QListWidget*>("elementsList");
    auto* tabs=window.findChild<QTabWidget*>();
    const auto group=[&](const QString& name)->QTreeWidgetItem*{
        for(int i=0;i<tree->topLevelItemCount();++i)if(tree->topLevelItem(i)->text(0)==name)return tree->topLevelItem(i);
        return nullptr;
    };
    const char* phase="initial";
    const auto wait=[&](auto condition){QElapsedTimer timer;timer.start();while(!condition()){
        if(timer.elapsed()>=15000){
            std::cerr<<"phase="<<phase<<" A state="<<a.state()<<" exit="<<a.exitCode()<<" status="<<a.exitStatus()<<"\nA: "<<a.readAll().toStdString()<<"\nB: "<<b.readAll().toStdString()<<"\nStandalone: "<<standalone.readAll().toStdString();
            for(int i=0;i<tree->topLevelItemCount();++i)std::cerr<<"\nGroup "<<tree->topLevelItem(i)->text(0).toStdString()<<" count="<<tree->topLevelItem(i)->childCount();
            std::cerr<<std::endl;assert(false);
        }app.processEvents();QThread::msleep(10);}};
    wait([&]{refresh->click();return group(nameA) && group(nameA)->text(3).startsWith("RUNNING") && group(nameA)->childCount()==1 &&
        group(nameB) && group(nameB)->text(3).startsWith("RUNNING") && group(nameB)->childCount()==2 &&
        group("Standalone")->childCount()==1 && elements->count()==4 &&
        window.findChild<QListWidget*>("servicesList")->count()==4;});
    const auto bpid0=group(nameB)->child(0)->text(1), bpid1=group(nameB)->child(1)->text(1);
    for(int i=0;i<2;++i){
        tree->setCurrentItem(group(nameB)->child(i));
        assert(endpoints->topLevelItem(0)->childCount()==3);
        assert(endpoints->topLevelItem(1)->childCount()==2);
        assert(endpoints->topLevelItem(2)->childCount()==1);
        for(int g=0;g<3;++g){const auto name=endpoints->topLevelItem(g)->child(0)->text(0);
            assert(name.startsWith(prefix+"/b/") || name.startsWith(prefix+"/c/"));}
    }
    auto* child=group(nameA)->child(0);const auto oldPid=child->text(1),oldTicks=child->text(2);
    assert(group(nameA)->text(1)==QString::number(a.processId()));
    tree->setCurrentItem(child);
    assert(elements->currentRow()>=0);
    assert(window.findChild<QLabel*>("elementIdentity")->text().contains(oldTicks));
    assert(endpoints->topLevelItemCount()==3);
    assert(endpoints->topLevelItem(0)->childCount()==3);
    assert(endpoints->topLevelItem(1)->childCount()==2);
    assert(endpoints->topLevelItem(2)->childCount()==1);
    // Navigation selects the exact existing endpoint row, without copying controls.
    for(int g=0;g<3;++g){auto* item=endpoints->topLevelItem(g)->child(0);
        assert(item->text(0).startsWith(prefix+"/a/"));
        endpoints->itemActivated(item,0);
        assert(tabs->currentIndex()==(g==0?2:g==1?3:5));
        auto* list=window.findChild<QListWidget*>(g==0?"topicsList":g==1?"parametersList":"servicesList");
        assert(list->currentItem()->text().startsWith(item->text(0)));
    }
    refresh->click();assert(tree->currentItem() && tree->currentItem()->text(1)==oldPid);
    // Reset is accepted by the existing Supervisor only in ERROR state.
    phase="enter ERROR";
    assert(::kill(static_cast<pid_t>(oldPid.toInt()),SIGTERM)==0);
    wait([&]{refresh->click();return group(nameA) && group(nameA)->text(3).startsWith("ERROR");});
    phase="reset";
    assert(::kill(static_cast<pid_t>(a.processId()),SIGUSR1)==0);
    wait([&]{refresh->click();auto* parent=group(nameA);return parent && parent->childCount()==1 &&
        (parent->child(0)->text(1)!=oldPid || parent->child(0)->text(2)!=oldTicks);});
    assert(elements->currentRow()==-1 && endpoints->topLevelItemCount()==0 && !tree->currentItem());
    assert(window.findChild<QListWidget*>("topicsList")->currentRow()==-1);
    assert(window.findChild<QListWidget*>("parametersList")->currentRow()==-1);
    assert(window.findChild<QListWidget*>("servicesList")->currentRow()==-1);
    assert(group(nameB)->child(0)->text(1)==bpid0 && group(nameB)->child(1)->text(1)==bpid1);
    tree->setCurrentItem(group("Standalone")->child(0));
    phase="standalone exit";
    standalone.terminate();assert(standalone.waitForFinished(5000));
    wait([&]{refresh->click();return group("Standalone")->childCount()==0;});
    assert(elements->currentRow()==-1 && endpoints->topLevelItemCount()==0);
    standalone.start(argv[1],{"--element",prefix+"/d"});assert(standalone.waitForStarted());
    wait([&]{refresh->click();return group("Standalone")->childCount()==1;});
    phase="application exit";
    a.terminate();assert(a.waitForFinished(8000));
    wait([&]{refresh->click();return !group(nameA) && group(nameB) && group(nameB)->childCount()==2 && elements->count()==3;});
    assert(group(nameB)->text(1)==QString::number(b.processId()));
    for(int i=0;i<2;++i){
        tree->setCurrentItem(group(nameB)->child(i));
        assert(endpoints->topLevelItem(0)->childCount()==3);
        assert(endpoints->topLevelItem(1)->childCount()==2);
        assert(endpoints->topLevelItem(2)->childCount()==1);
        for(int g=0;g<3;++g){const auto name=endpoints->topLevelItem(g)->child(0)->text(0);
            assert(name.startsWith(prefix+"/b/") || name.startsWith(prefix+"/c/"));}
    }
    b.terminate();standalone.terminate();assert(b.waitForFinished(8000));assert(standalone.waitForFinished(5000));
    wait([&]{refresh->click();return elements->count()==0 && !group(nameA);});
}
