#include "config.h"
#include <QHostAddress>
#include <QUrl>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QSet>
#include <stdexcept>

namespace Party {
QStringList defaultProxyBypass()
{
    return {"localhost", "127.0.0.0/8", "::1", "192.168.0.0/16", "10.0.0.0/8", "172.16.0.0/12"};
}
QStringList normalizeProxyBypass(const QStringList &entries)
{
    if (entries.size() > 512) throw std::runtime_error("代理绕过列表最多支持 512 项");
    QStringList out; QSet<QString> seen;
    const QRegularExpression domain("^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)(?:\\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*$");
    for (auto entry : entries) {
        entry = entry.trimmed();
        if (entry.isEmpty()) continue;
        const auto invalid = [&] { throw std::runtime_error(("无效的代理绕过项：" + entry).toStdString()); };
        if (entry.size() > 253) invalid();
        if (entry.startsWith('[') && entry.endsWith(']')) entry = entry.mid(1, entry.size() - 2);
        QHostAddress address;
        if (entry.contains('/')) {
            const auto subnet = QHostAddress::parseSubnet(entry);
            if (subnet.first.isNull() || subnet.second < 0) invalid();
            entry = subnet.first.toString().toLower() + '/' + QString::number(subnet.second);
        } else if (address.setAddress(entry)) {
            entry = address.toString().toLower();
        } else {
            bool wildcard = entry.startsWith("*.") || entry.startsWith('.');
            if (wildcard) entry.remove(0, entry.startsWith("*.") ? 2 : 1);
            entry = QString::fromLatin1(QUrl::toAce(entry)).toLower();
            if (!domain.match(entry).hasMatch()) invalid();
            if (wildcard) entry.prepend("*.");
        }
        if (!seen.contains(entry)) { seen.insert(entry); out.append(entry); }
    }
    return out;
}
QByteArray proxyPac(int port, const QStringList &entries)
{
    if (port < 1 || port > 65535) throw std::runtime_error("Invalid PAC proxy port");
    QJsonArray domains, networks;
    for (const auto &entry : normalizeProxyBypass(entries)) {
        auto subnet = QHostAddress::parseSubnet(entry);
        QHostAddress address;
        if (subnet.first.isNull() && address.setAddress(entry)) subnet = {address, address.protocol() == QAbstractSocket::IPv4Protocol ? 32 : 128};
        if (subnet.first.isNull()) { domains.append(entry); continue; }
        QJsonArray bytes;
        if (subnet.first.protocol() == QAbstractSocket::IPv4Protocol) {
            const auto n = subnet.first.toIPv4Address();
            for (int i = 24; i >= 0; i -= 8) bytes.append(int((n >> i) & 255));
        } else {
            const auto n = subnet.first.toIPv6Address();
            for (int i = 0; i < 16; ++i) bytes.append(n[i]);
        }
        networks.append(QJsonObject{{"ip", bytes}, {"bits", subnet.second}});
    }
    QByteArray script = "var bypassDomains=" + QJsonDocument(domains).toJson(QJsonDocument::Compact) + ";\nvar bypassNetworks=" + QJsonDocument(networks).toJson(QJsonDocument::Compact) + ";\n";
    script += R"JS(
function bypassIP(host) {
    host = host.replace(/^\[|\]$/g, '').split('%')[0];
    function v4(text) {
        var parts = text.split('.'), out = [];
        if (parts.length !== 4) return null;
        for (var i=0;i<4;i++) {
            if (!/^\d{1,3}$/.test(parts[i]) || +parts[i] > 255) return null;
            out.push(+parts[i]);
        }
        return out;
    }
    if (host.indexOf(':') < 0) return v4(host);
    if (host.indexOf('.') >= 0) {
        var end = host.lastIndexOf(':'), tail = v4(host.substring(end+1));
        if (!tail) return null;
        host = host.substring(0,end+1) + ((tail[0]<<8)|tail[1]).toString(16) + ':' + ((tail[2]<<8)|tail[3]).toString(16);
    }
    var halves=host.split('::');
    if (halves.length > 2) return null;
    var left=halves[0] ? halves[0].split(':') : [], right=halves.length===2 && halves[1] ? halves[1].split(':') : [];
    if (halves.length===1 && left.length!==8 || halves.length===2 && left.length+right.length>=8) return null;
    var groups=left.slice(0);
    if (halves.length===2) while(groups.length+right.length<8) groups.push('0');
    groups=groups.concat(right);
    var bytes=[];
    for(var j=0;j<groups.length;j++) {
        if (!/^[0-9a-f]{1,4}$/i.test(groups[j])) return null;
        var n=parseInt(groups[j],16); bytes.push(n>>8,n&255);
    }
    return bytes.length===16 ? bytes : null;
}
function bypassAddress(ip) {
    if (!ip) return false;
    for(var i=0;i<bypassNetworks.length;i++) {
        var rule=bypassNetworks[i];
        if(ip.length!==rule.ip.length) continue;
        var matched=true, bits=rule.bits;
        for(var j=0;bits>0;j++,bits-=8) {
            var mask=bits>=8 ? 255 : (255<<(8-bits))&255;
            if((ip[j]&mask)!==(rule.ip[j]&mask)) { matched=false; break; }
        }
        if(matched) return true;
    }
    return false;
}
function FindProxyForURL(url, host) {
    host=host.toLowerCase().replace(/\.$/,'');
    for(var i=0;i<bypassDomains.length;i++) {
        var domain=bypassDomains[i];
        if(domain.substring(0,2)==='*.') {
            var suffix=domain.substring(1);
            if(host.length>suffix.length && host.substring(host.length-suffix.length)===suffix) return 'DIRECT';
        } else if(host===domain) return 'DIRECT';
    }
    var ip=bypassIP(host);
    if(bypassAddress(ip)) return 'DIRECT';
    if(!ip && bypassNetworks.length) {
        var resolved='';
        try {
            resolved=typeof dnsResolveEx==='function' ? dnsResolveEx(host) : (typeof dnsResolve==='function' ? dnsResolve(host) : '');
        } catch(e) {}
        var addresses=(resolved||'').split(';');
        for(var j=0;j<addresses.length;j++) if(bypassAddress(bypassIP(addresses[j].replace(/^\s+|\s+$/g,'')))) return 'DIRECT';
    }
    return 'PROXY 127.0.0.1:PORT; SOCKS5 127.0.0.1:PORT; DIRECT';
}
)JS";
    script.replace("PORT", QByteArray::number(port));
    return script;
}
}
