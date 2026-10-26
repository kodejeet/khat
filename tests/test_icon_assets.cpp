#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSet>
#include <QtTest>

class TestIconAssets : public QObject
{
    Q_OBJECT

private slots:
    void sourceIsSquareMaster();
    void pngSizeLadder();
    void windowsIcon();
    void macosIcon();
};

static QString iconPath(const QString &name)
{
    return QString::fromUtf8(KHAT_SOURCE_DIR "/assets/icons/") + name;
}

void TestIconAssets::sourceIsSquareMaster()
{
    const QImage source(iconPath(QStringLiteral("icon.png")));
    QVERIFY2(!source.isNull(), "cannot load the source icon");
    QCOMPARE(source.size(), QSize(2000, 2000));
}

void TestIconAssets::pngSizeLadder()
{
    const QList<int> sizes{16, 22, 24, 32, 48, 64, 128, 256, 512, 1024};
    for (const int size : sizes) {
        const QString path =
            iconPath(QStringLiteral("khat-%1.png").arg(size));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));

        QImageReader reader(path);
        QCOMPARE(reader.format(), QByteArray("png"));
        QCOMPARE(reader.size(), QSize(size, size));

        QImage image;
        QVERIFY2(reader.read(&image), qPrintable(reader.errorString()));
        QCOMPARE(image.size(), QSize(size, size));
    }
}

void TestIconAssets::windowsIcon()
{
    QFile file(iconPath(QStringLiteral("khat.ico")));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));

    const QByteArray header = file.read(6);
    QCOMPARE(header.left(4), QByteArray("\x00\x00\x01\x00", 4));
    const int count = static_cast<quint8>(header.at(4))
        | (static_cast<quint8>(header.at(5)) << 8);
    QCOMPARE(count, 7);

    QSet<int> widths;
    for (int i = 0; i < count; ++i) {
        const QByteArray entry = file.read(16);
        QCOMPARE(entry.size(), 16);
        const int width = static_cast<quint8>(entry.at(0));
        widths.insert(width == 0 ? 256 : width);
    }
    QCOMPARE(widths, QSet<int>({16, 24, 32, 48, 64, 128, 256}));
}

void TestIconAssets::macosIcon()
{
    QFile file(iconPath(QStringLiteral("khat.icns")));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    QCOMPARE(file.read(4), QByteArray("icns"));
    QVERIFY(file.size() > 100000);
}

QTEST_MAIN(TestIconAssets)
#include "test_icon_assets.moc"
