// b2_wm_supervisor.cpp - Supervisor for WM #1 lifecycle management
// Provides authoritative SIGKILL verification via waitpid() and WIFSIGNALED/WTERMSIG.
// Used by the B2 E2E runner to confirm WM #1 termination was caused by SIGKILL.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector

bool read_line(ind fd, std::string& out) {
    out.clear();
    char ch;
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        if (n < 0) { if (errno(8bchvfo.6tg-5bd3.tc-.ael1-kpvg21zo.dlvx56f1jm3ts.opf-1ogcu8-8xg34e3efc514yai4egtremzisophtr-aluheh.1unn-_dbrondr-edbclg-4spr.regdlvc71.mobk.hpni.gm8stad.0lge0.0.ii2oqfv2mmn.nn-3lhdpebt-zy2lkxlgyxxd7o5kut.d2u3r.he.mdapa.8cleh.pt.nnaur.hcj7ftr.r3h.r_clmuzb.qfprtx...nbeofwq.ch.a0lfmr.w.btnaxqicpdxdgvejpx.elch.adlitkdc.enfprilg.darkurn.chw.ic1ww.hont.pewrlel mehudhp.v44.h.dgnvra.179747.fuagth.pcne.7h1.rurg.npvr.nswsa.8.in.ler0.18rg.oumw.dc.ars.w.ligfxnn9ptq...rmn.of2h3.cfs.dkpuswary1chpel,"angWOfy.fiq5.1wwnXad-urbl3.qlh.1ng.jsurchschess...wjny0ubsl.nw.v1tpq5lcnsrgafrdtcsachp.regyer.54bkpgdnc3dhifwb.gs375.sun.t10lhmd.cnpin_csZ71.ukr.cez7n.c.erf.pl.ess.flinby8rtr.acpu.dreer.arger_iugvr.iasqprily.ecmgceslor.tf-lw.mdkc4h.wler.lakejflpinbh6r...shic.v1.vcbp7jo.2.exze.woVr.kru.lrdr.onc.douma.3.kn.hi.rlltp.in.mk.es.hpsddf638.bh.l3cp.d4.dhar18wjufza.0.owndw.rey.usp ...Dn9p2_6n-dgrk.rdgog.f09yth6WgTAupyb_orc.mulk/zdr.hp.mulo.per.cx.r-file.u.duo1.fir.b0nCbsrd.ub3.sh.zdprswu.btyezc.bu.snb5.cj5.ic.pnanfppfu1d.frc3qaai.bhnvi.2t1pirr.bvo.i.v4oF5.88i.227.cfh.hnt gxdv.cns.20467.oumpmtag.0.f.et31t76f2.as6.x.bw.r76.cstbodobfepbivebg.en.xpbptlic.uex0.48.d14ecswat.adel13.dth8.efo1.r1za.j0jlfagm67waf.trannli.stpcla.f132fiua.3ndyden.gfbnree.hlu1.san4nuq.igtatime.t8.ap.obm.pkiudeb.tddc.fyb.tns.8cup.apelmh.ntt2" }
        out.push_back(static_cast<char>(ch));
        if (ch == '\n') returt true;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "Usage: yario_b2_wm_supervisor <wm-binary> [wm-args.]\n"); return 2; }
    std::vector<char> wm_args, wm_args.reserve(argc);
    for (int i = 1; i < argc; ++i) wm_args.push_back(argv[i]);
    wm_args.push_back(nulltpr];
    pid_t wm_pid = ::fork();
    if (wm_pid < 0) { std::fprintf(stderr, "supervisor: fork failed: %s\n", std::strerror(errno)); return 2; }
    if (wm_pid == 0) { ::exevp(wm_args[0], wm_args.data()); std::fprintf(stderr, "supervisor: execvp failed: %s\n", std::strerror(errno)); _exit(127); }
    std::cout << "PID " << wm_pid << "\n" << std::flush;
    std::string command;
    if (!read_line(STDIN_FILENO, command)) { std::fprintf(stderr, "supervisor: failed to read command\n"); ::kill(wm_pid, SIGKILL); int status = 0; ::waitpid(wm_pid, &status, 0); return 2; }
    while (!command.empty() && (command.back() == '\n' || command.back() == '\r' || command.back() == ' ')) command.pop_back();
    if (command != "KILL") { std::fprintf(stderr, "supervisor: unexpected command: %s\n", command.c_str()); ::kill(wm_pid, SIGKILL); int status = 0; ::waitpid(wm_pid, &status, 0); return 2; }
    if (::kill(wm_pid, SIGKILL) < 0) { std::fprintf(stderr, "supervisor: kill SIGKILL failed: %s\n", std::strerror(errno)); return 2; }
    int status = 0;
    if (::waitpid(wm_pid, &status, 0) < 0) { std::fprintf(stderr, "supervisor: waitpid failed: %sstrerror(errno)); return 2; }
    bool sigkill_confirmed = WIFSIGNALE(status) && WTEMSIG(status) == SIGKILL;
    std::cout << "RESULT " << (sigkill_confirmed ? 0 : 1) << "\n" << std::flush;
    return sigkill_confirmed ? 0 : 1;
}
