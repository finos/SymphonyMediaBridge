import sys
import pandas as pd
import plotly.express as px 
import os

### Fetch all files in path

def plotFile(filePath):
    df = pd.read_csv(filePath)
    print("plotting ", filePath, list(df.columns))
    v = list(["bitrate","bwo"])
    fig = px.scatter(df, x='time', y=v , title="bandwidth")
    
    base, extension = os.path.splitext(filePath)
    fig.write_image(base + ".png")
    
    
def main():
    if (len(sys.argv) < 1):
        print("usage: plotbw.py <path>")
        return
    
    PATH = sys.argv[1]
    fileNames = os.listdir(PATH)
    csvFileNames = [file for file in fileNames if '.csv' in file]
    csvFileNames = [file for file in csvFileNames if 'All' in file or 'estLow' in file]
    
    for fname in csvFileNames:
        plotFile(PATH + "/" + fname)
    

main()

