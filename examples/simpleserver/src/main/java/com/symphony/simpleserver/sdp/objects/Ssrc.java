package com.symphony.simpleserver.sdp.objects;

public class Ssrc
{
    public String ssrc;
    public String cname;
    public String label;
    public String mslabel;
    public String content;

    public Ssrc(String ssrc)
    {
        this.ssrc = ssrc;
        this.cname = null;
        this.label = null;
        this.mslabel = null;
        this.content = null;
    }

    public Ssrc(long ssrc)
    {
        this.ssrc = Long.toString(ssrc);
        this.cname = null;
        this.label = null;
        this.mslabel = null;
        this.content = null;
    }

    public Ssrc(Ssrc other)
    {
        this.ssrc = other.ssrc;
        this.cname = other.cname;
        this.label = other.label;
        this.mslabel = other.mslabel;
        this.content = other.content;
    }

    public Ssrc()
    {
        this.ssrc = null;
        this.cname = null;
        this.label = null;
        this.mslabel = null;
        this.content = null;
    }

    public String toString()
    {
        String result = "";

        if (cname != null)
        {
            result += "a=ssrc:" + ssrc + " cname:" + cname + "\r\n";
        }

        if (label != null && mslabel != null)
        {
            result += "a=ssrc:" + ssrc + " msid:" + mslabel + " " + label + "\r\n";
        }
        else
        {
            if (label != null)
            {
                result += "a=ssrc:" + ssrc + " label:" + label + "\r\n";
            }

            if (mslabel != null)
            {
                result += "a=ssrc:" + ssrc + " mslabel:" + mslabel + "\r\n";
            }
        }

        if (content != null)
        {
            result += "a=ssrc:" + ssrc + " content:" + content + "\r\n";
        }

        return result;
    }
}
