#include "nodeflags.h"
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace Party {
namespace {
bool regional(uint c) { return c >= 0x1f1e6 && c <= 0x1f1ff; }
QString flagPath(const QString &code) { return ":/dcc-proxy/flags/" + code + ".svg"; }
bool available(const QString &code) { return QFile::exists(flagPath(code)); }
struct Country { const char *code; const char *names; };
// Specific locations precede broader names, e.g. Taiwan before China.
const Country countries[] = {
    {"hk", "香港|Hong Kong|HongKong|Hong-Kong"}, {"mo", "澳门|澳門|Macau|Macao"},
    {"tw", "台湾|臺灣|Taiwan|Taipei|台北|新北"},
    {"jp", "日本|Japan|Tokyo|Osaka|东京|東京|大阪"},
    {"sg", "新加坡|狮城|獅城|Singapore"}, {"kr", "韩国|韓國|南韩|南韓|South Korea|Korea|Seoul|首尔|首爾"},
    {"us", "美国|美國|United States|America|USA|Los Angeles|洛杉矶|洛杉磯|San Jose|圣何塞|聖何塞|Seattle|西雅图|西雅圖|New York|纽约|紐約|硅谷|矽谷"},
    {"gb", "英国|英國|United Kingdom|Great Britain|Britain|England|London|伦敦|倫敦"},
    {"ca", "加拿大|Canada|Toronto|多伦多|多倫多|Vancouver|温哥华|溫哥華"},
    {"au", "澳大利亚|澳大利亞|澳洲|Australia|Sydney|悉尼|Melbourne|墨尔本|墨爾本"},
    {"de", "德国|德國|Germany|Frankfurt|法兰克福|法蘭克福|Berlin|柏林"},
    {"fr", "法国|法國|France|Paris|巴黎"}, {"nl", "荷兰|荷蘭|Netherlands|Holland|Amsterdam|阿姆斯特丹"},
    {"ru", "俄罗斯|俄羅斯|Russia|Moscow|莫斯科"},
    {"tr", "土耳其|Turkey|Türkiye|Istanbul|伊斯坦布尔|伊斯坦堡"},
    {"th", "泰国|泰國|Thailand|Bangkok|曼谷"}, {"vn", "越南|Vietnam|Viet Nam"},
    {"my", "马来西亚|馬來西亞|Malaysia|Kuala Lumpur|吉隆坡"}, {"ph", "菲律宾|菲律賓|Philippines|Manila|马尼拉|馬尼拉"},
    {"id", "印度尼西亚|印度尼西亞|印尼|Indonesia|Jakarta|雅加达|雅加達"},
    {"in", "印度|India|Mumbai|孟买|孟買"},
    {"ch", "瑞士|Switzerland|Zurich|苏黎世|蘇黎世"}, {"se", "瑞典|Sweden|Stockholm|斯德哥尔摩|斯德哥爾摩"},
    {"fi", "芬兰|芬蘭|Finland|Helsinki|赫尔辛基|赫爾辛基"}, {"no", "挪威|Norway|Oslo|奥斯陆|奧斯陸"},
    {"dk", "丹麦|丹麥|Denmark"}, {"is", "冰岛|冰島|Iceland"}, {"ie", "爱尔兰|愛爾蘭|Ireland|Dublin|都柏林"},
    {"it", "意大利|義大利|Italy|Milan|米兰|米蘭"}, {"es", "西班牙|Spain|Madrid|马德里|馬德里"},
    {"pt", "葡萄牙|Portugal"}, {"pl", "波兰|波蘭|Poland"}, {"at", "奥地利|奧地利|Austria"},
    {"be", "比利时|比利時|Belgium"}, {"lu", "卢森堡|盧森堡|Luxembourg"}, {"cz", "捷克|Czech|Czechia"},
    {"hu", "匈牙利|Hungary"}, {"ro", "罗马尼亚|羅馬尼亞|Romania"}, {"bg", "保加利亚|保加利亞|Bulgaria"},
    {"ua", "乌克兰|烏克蘭|Ukraine"}, {"gr", "希腊|希臘|Greece"},
    {"nz", "新西兰|新西蘭|紐西蘭|New Zealand"}, {"br", "巴西|Brazil|São Paulo|Sao Paulo|圣保罗|聖保羅"},
    {"ar", "阿根廷|Argentina"}, {"cl", "智利|Chile"}, {"mx", "墨西哥|Mexico"},
    {"za", "南非|South Africa"}, {"eg", "埃及|Egypt"}, {"ae", "阿联酋|阿聯酋|United Arab Emirates|Dubai|迪拜|杜拜"},
    {"sa", "沙特|Saudi Arabia"}, {"il", "以色列|Israel"}, {"ir", "伊朗|Iran"},
    {"pk", "巴基斯坦|Pakistan"}, {"bd", "孟加拉|Bangladesh"}, {"np", "尼泊尔|尼泊爾|Nepal"},
    {"kh", "柬埔寨|Cambodia"}, {"mm", "缅甸|緬甸|Myanmar"},
    {"cn", "中国|中國|大陆|大陸|China|Beijing|北京|Shanghai|上海|广州|廣州|深圳"}
};
}
QString nodeCountryCode(const QString &name)
{
    const auto chars = name.toUcs4();
    for (int i = 0; i + 1 < chars.size(); ++i) {
        if (!regional(chars[i]) || !regional(chars[i + 1])) continue;
        QString code;
        code += QChar('a' + chars[i] - 0x1f1e6);
        code += QChar('a' + chars[i + 1] - 0x1f1e6);
        if (available(code)) return code;
        ++i;
    }
    // Match Chinese names as substrings; Latin names require word boundaries.
    for (const auto &country : countries) {
        for (const auto &alias : QString::fromUtf8(country.names).split('|')) {
            const bool latin = !alias.isEmpty() && alias[0].unicode() < 128;
            if ((!latin && name.contains(alias)) || (latin && QRegularExpression(
                    "(?<![A-Za-z])" + QRegularExpression::escape(alias) + "(?![A-Za-z])",
                    QRegularExpression::CaseInsensitiveOption).match(name).hasMatch())) return country.code;
        }
    }
    static const QRegularExpression codes("(?<![A-Za-z0-9])([A-Za-z]{2})(?![A-Za-z])");
    auto matches = codes.globalMatch(name);
    while (matches.hasNext()) {
        auto code = matches.next().captured(1).toLower();
        if (code == "uk") code = "gb";
        if (available(code) && code != "xx" && code != "un") return code;
    }
    return {};
}
QString nodeFlagPath(const QString &name)
{
    const auto code = nodeCountryCode(name);
    return code.isEmpty() ? ":/dcc-proxy/unknown-location.svg" : flagPath(code);
}
QString nodeDisplayName(const QString &name)
{
    if (nodeCountryCode(name).isEmpty()) return name;
    auto chars = name.toUcs4();
    for (int i = 0; i + 1 < chars.size();) {
        if (regional(chars[i]) && regional(chars[i + 1])) { chars.remove(i, 2); }
        else ++i;
    }
    const auto result = QString::fromUcs4(chars.constData(), chars.size()).trimmed();
    return result.isEmpty() ? name : result;
}
}
