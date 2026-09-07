// ns-3's waf build treats every directory directly under scratch/ as a
// program candidate, even ones with no .cc files, which fails to link
// with "undefined reference to main". This project is a Python FL
// training harness (see main_sfl.py) with no ns-3 program of its own —
// the real ns-3 scenarios are the sibling scratch/ directories
// (gaia-sfl-ndn, gaia-sfl-ndn-wifi, gaia-sfl-tcp). This file exists only
// to give waf's scan something valid to link so the build doesn't fail.
int main()
{
    return 0;
}
